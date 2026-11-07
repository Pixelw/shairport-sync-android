/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Pixelw and contributors
 */
#include "companion.h"
#include "ap2_event_receiver.h"
#include "utilities/general_utilities.h"
#include "common.h"
#include "metadata/hub.h"
#include "remote/remote.h"
#include "worker.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <sys/un.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>

#define MAX_ART (4U * 1024U * 1024U)
#define VOLUME_SOCKET_NAME "org.shairport.sync.android.volume.v1"
#define VOLUME_MAGIC 0x53505631U
#define VOLUME_SCALE 1000000U
#define SOCKET_NAME "org.shairport.sync.android.companion.v1"
extern pthread_rwlock_t metadata_hub_re_lock;
static pthread_mutex_t image_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t timing_lock = PTHREAD_MUTEX_INITIALIZER;
static char *image;
static size_t image_length;
static uint64_t image_generation;
static int timing_connection;
static uint64_t timing_frames;
static uint32_t timing_timestamp;
static atomic_int enabled;
static int started;
static uid_t allowed_uid;
static pthread_t thread, volume_thread, volume_events_thread;
static atomic_int volume_enabled;
static int volume_events_started;
static char instance[80];
/* 仅桥接线程拥有这些资源，根清理器统一释放。 */
static int listener = -1, client = -1;
static char *outgoing;
static char *json;
static char last_session[100];

int sps_companion_picture(const char *data, size_t length) {
  if (!enabled) return 0;
  if (!data || length > MAX_ART) length = 0;
  char *copy = length ? malloc(length) : NULL;
  if (length && !copy) return 0;
  if (length) memcpy(copy, data, length);
  pthread_mutex_lock(&image_lock);
  int changed = length != image_length || (length && memcmp(image, copy, length));
  if (changed) {
    free(image); image = copy; image_length = length; ++image_generation;
  } else free(copy);
  pthread_mutex_unlock(&image_lock);
  return changed;
}

void sps_companion_timing(int connection, uint64_t frames, uint32_t timestamp) {
  if (!enabled || pthread_mutex_trylock(&timing_lock)) return;
  timing_connection = connection; timing_frames = frames; timing_timestamp = timestamp;
  pthread_mutex_unlock(&timing_lock);
}

uint64_t sps_companion_frames(int connection) {
  pthread_mutex_lock(&timing_lock);
  uint64_t frames = timing_connection == connection ? timing_frames : 0;
  pthread_mutex_unlock(&timing_lock);
  return frames;
}

static void cleanup(void *unused) {
  (void)unused;
  if (client >= 0) close(client);
  if (listener >= 0) close(listener);
  listener = client = -1;
  free(outgoing); outgoing = NULL;
  free(json); json = NULL;
}

static int64_t monotonic_ms(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return now.tv_sec * 1000LL + now.tv_nsec / 1000000;
}

static int transfer_until(int fd, void *data, size_t length, int writing, int64_t deadline) {
  size_t done = 0;
  while (done < length) {
    int64_t left = deadline - monotonic_ms();
    if (left <= 0) return -1;
    struct pollfd p = {.fd = fd, .events = writing ? POLLOUT : POLLIN};
    if (lx_worker_poll(&p, 1, left) <= 0 || (p.revents & (POLLERR | POLLNVAL))) return -1;
    ssize_t n = writing ? send(fd, (char *)data + done, length - done, MSG_NOSIGNAL) :
                          recv(fd, (char *)data + done, length - done, 0);
    if (n > 0) done += n;
    else if (n == 0 || (errno != EAGAIN && errno != EINTR)) return -1;
  }
  return 0;
}

/* 元数据桥接沿用原有的单次传输时限。 */
static int transfer(int fd, void *data, size_t length, int writing) {
  return transfer_until(fd, data, length, writing, monotonic_ms() + 2000);
}

int sps_companion_system_volume(int request, double *volume) {
  if (!volume_enabled) return 0;
  int result = 0, prior;
  lx_worker_defer(LX_STOP_DEFERRED, &prior);
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd >= 0) {
    int64_t deadline = monotonic_ms() + 200;
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    memcpy(addr.sun_path + 1, VOLUME_SOCKET_NAME, sizeof(VOLUME_SOCKET_NAME) - 1);
    struct ucred peer;
    socklen_t peer_size = sizeof(peer);
    uint32_t message[] = {htonl(VOLUME_MAGIC), htonl((uint32_t)request)};
    /* 抽象 Unix stream 在监听队列已满时返回 EAGAIN，直接回退。 */
    if (!connect(fd, (struct sockaddr *)&addr,
                 offsetof(struct sockaddr_un, sun_path) + sizeof(VOLUME_SOCKET_NAME)) &&
        !getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) && peer.uid == allowed_uid &&
        !transfer_until(fd, message, sizeof(message), 1, deadline) &&
        !transfer_until(fd, message, sizeof(message), 0, deadline) &&
        ntohl(message[0]) == VOLUME_MAGIC && ntohl(message[1]) <= VOLUME_SCALE) {
      uint32_t value = ntohl(message[1]);
      *volume = value == 0 ? -144.0 : 30.0 * value / VOLUME_SCALE - 30.0;
      result = 1;
    }
    close(fd);
  }
  lx_worker_defer(prior, NULL);
  return result;
}

static void *poll_volume(void *unused) {
  (void)unused;
  lx_worker_ready();
  for (;;) {
    lx_worker_poll(NULL, 0, 250);
    if (pthread_rwlock_tryrdlock(&principal_conn_lock)) continue;
    pthread_cleanup_push(rwlock_unlock, &principal_conn_lock);
    if (principal_conn) player_poll_system_volume(principal_conn);
    pthread_cleanup_pop(1);
  }
  return NULL;
}

static void *send_volume_events(void *unused) {
  (void)unused;
  lx_worker_ready();
  ap2_event_companion_mode(1);
  for (;;) {
    lx_worker_poll(NULL, 0, 100);
    if (pthread_rwlock_tryrdlock(&principal_conn_lock)) continue;
    pthread_cleanup_push(rwlock_unlock, &principal_conn_lock);
    double volume;
    if (principal_conn && principal_conn->airplay_type == ap_2 &&
        principal_conn->event_channel_fd > 0 &&
        player_take_system_volume_notification(principal_conn, &volume))
      ap2_event_send_unit_volume_notification(principal_conn, airplayVolumeToUnitVolume(volume));
    pthread_cleanup_pop(1);
  }
  return NULL;
}

static int packet(unsigned type, const void *data, uint32_t length) {
  uint32_t header[] = {htonl(0x53505331), htonl(type), htonl(length)};
  return transfer(client, header, sizeof(header), 1) ||
         (length && transfer(client, (void *)data, length, 1)) ? -1 : 0;
}

static void str(plist_t p, const char *key, const char *value) {
  /* 字符串有界，防止发送端元数据放大桥接快照。 */
  char *bounded = strndup(value ? value : "", 8192);
  if (bounded) { plist_dict_set_item(p, key, plist_new_string(bounded)); free(bounded); }
}
static void num(plist_t p, const char *key, uint64_t value) {
  plist_dict_set_item(p, key, plist_new_uint(value));
}

static uint32_t commands(const metadata_bundle *m, int available) {
  if (!available) return 0;
  if (!m->supported_commands_plist) return 0x33; /* play/pause/next/previous，尚未声明则尝试。 */
  uint32_t mask = 0;
  for (uint32_t i = 0; i < plist_array_get_size(m->supported_commands_plist) && i < 128; ++i) {
    char *data = NULL; uint64_t length = 0;
    plist_get_data_val(plist_array_get_item(m->supported_commands_plist, i), &data, &length);
    plist_t info = NULL;
    if (data && length <= 65536) plist_from_memory(data, (uint32_t)length, &info, NULL);
    free(data);
    if (info) {
      uint64_t command = 64; uint8_t on = 1;
      plist_t item = plist_dict_get_item(info, "kCommandInfoCommandKey");
      if (item) plist_get_uint_val(item, &command);
      item = plist_dict_get_item(info, "kCommandInfoEnabledKey");
      if (item) plist_get_bool_val(item, &on);
      if (on && command < 6) mask |= 1U << command;
      plist_free(info);
    }
  }
  return mask & 0x33;
}

/* 用 try-lock 获取完整快照，不让 UI 反向阻塞实时工作；此处不执行 socket IO。 */
static plist_t snapshot(void) {
  if (pthread_rwlock_tryrdlock(&principal_conn_lock)) return NULL;
  if (pthread_rwlock_tryrdlock(&metadata_hub_re_lock)) {
    pthread_rwlock_unlock(&principal_conn_lock); return NULL;
  }
  plist_t p = plist_new_dict();
  metadata_bundle *m = &metadata_store;
  rtsp_conn_info *conn = principal_conn;
  int present = conn && conn->airplay_type == ap_2;
  if (present) snprintf(last_session, sizeof(last_session), "%s-%d", instance, conn->connection_number);
  str(p, "session", last_session);
  int paused = m->play_status == PS_PAUSED || m->player_state == PS_PAUSED;
  int stopped = !present || m->play_status == PS_STOPPED ||
                (!paused && m->player_state == PS_STOPPED);
  const char *state = stopped ? "stopped" : paused ? "paused" :
      (m->player_state == PS_PLAYING || m->play_status == PS_PLAYING) ? "playing" : "idle";
  str(p, "state", state);
  str(p, "title", m->npi.track_name); str(p, "artist", m->npi.artist_name);
  str(p, "album", m->npi.album_name); str(p, "device", m->client_name);
  uint64_t position = 0, duration = 0;
  int valid = 0;
  if (m->npi.songtime_in_microseconds.valid) duration = m->npi.songtime_in_microseconds.value / 1000;
  pthread_mutex_lock(&timing_lock);
  if (present && timing_connection == conn->connection_number) {
    if (m->npi.elapsedTimeOnReceiptOfNowPlayingInfo.valid) {
      position = m->npi.elapsedTimeOnReceiptOfNowPlayingInfo.value / 1000000;
      unsigned rate = RATE_FROM_ENCODED_FORMAT(config.current_output_configuration);
      uint64_t base = m->npi.framesPlayedOnReceiptOfNowPlayingInfo;
      if (rate && timing_frames >= base && m->npi.playbackRate > 0)
        position += (timing_frames - base) * 1000 / rate;
      valid = 1;
    } else if (conn->input_rate && m->progress_string && m->npi.playing_state != 2) {
      int32_t total = m->progress_last_timestamp - m->progress_first_timestamp;
      int32_t played = timing_timestamp - m->progress_first_timestamp;
      if (total >= 0 && played >= 0 && played <= total) {
        position = (uint64_t)played * 1000 / conn->input_rate;
        duration = (uint64_t)total * 1000 / conn->input_rate; valid = 1;
      }
    }
  }
  pthread_mutex_unlock(&timing_lock);
  if (duration && position > duration) position = duration;
  num(p, "connection", present ? conn->connection_number : 0);
  num(p, "positionMs", position); num(p, "durationMs", duration);
  num(p, "positionValid", valid);
  num(p, "controls", commands(m, present && !stopped && conn->event_channel_fd > 0));
  pthread_rwlock_unlock(&metadata_hub_re_lock);
  pthread_rwlock_unlock(&principal_conn_lock);
  pthread_mutex_lock(&image_lock);
  num(p, "cover", image_length ? image_generation : 0);
  pthread_mutex_unlock(&image_lock);
  return p;
}

static int send_snapshot(uint64_t *sent_cover) {
  int prior;
  lx_worker_defer(LX_STOP_DEFERRED, &prior);
  plist_t p = snapshot();
  uint64_t cover = 0;
  uint32_t length = 0;
  if (p) {
    plist_get_uint_val(plist_dict_get_item(p, "cover"), &cover);
    plist_to_json(p, &json, &length, 0); plist_free(p);
  }
  lx_worker_defer(prior, NULL);
  if (!json) return 0;
  int result = packet(1, json, length);
  free(json); json = NULL;
  if (result || !cover || cover == *sent_cover) return result;
  lx_worker_defer(LX_STOP_DEFERRED, &prior);
  pthread_mutex_lock(&image_lock);
  uint32_t size = (cover == image_generation) ? (uint32_t)image_length : 0;
  outgoing = size ? malloc(size + 8) : NULL;
  if (outgoing) {
    uint32_t hi = htonl(cover >> 32), lo = htonl(cover);
    memcpy(outgoing, &hi, 4); memcpy(outgoing + 4, &lo, 4);
    memcpy(outgoing + 8, image, size);
  }
  pthread_mutex_unlock(&image_lock);
  lx_worker_defer(prior, NULL);
  if (outgoing) { result = packet(2, outgoing, size + 8); *sent_cover = cover; }
  free(outgoing); outgoing = NULL;
  return result;
}

static int receive_command(void) {
  uint32_t h[3];
  if (transfer(client, h, sizeof(h), 0) || ntohl(h[0]) != 0x53505331 || ntohl(h[1]) != 3 ||
      ntohl(h[2]) > 1024 || !ntohl(h[2])) return -1;
  char request[1025]; unsigned length = ntohl(h[2]);
  if (transfer(client, request, length, 0)) return -1;
  plist_t p = NULL;
  plist_from_json(request, length, &p);
  if (!p || plist_get_node_type(p) != PLIST_DICT) { if (p) plist_free(p); return -1; }
  char *session = NULL, *command = NULL;
  uint64_t id = 0;
  plist_get_string_val(plist_dict_get_item(p, "session"), &session);
  plist_get_string_val(plist_dict_get_item(p, "command"), &command);
  plist_get_uint_val(plist_dict_get_item(p, "id"), &id);
  int number = -1;
  if (command) {
    if (!strcmp(command, "play")) number = 0;
    if (!strcmp(command, "pause")) number = 1;
    if (!strcmp(command, "next")) number = 4;
    if (!strcmp(command, "previous")) number = 5;
  }
  plist_free(p);
  int prior; lx_worker_defer(LX_STOP_DEFERRED, &prior);
  p = snapshot();
  char *current = NULL; uint64_t mask = 0, connection = 0;
  if (p) {
    plist_get_string_val(plist_dict_get_item(p, "session"), &current);
    plist_get_uint_val(plist_dict_get_item(p, "controls"), &mask);
    plist_get_uint_val(plist_dict_get_item(p, "connection"), &connection); plist_free(p);
  }
  int valid = current && session && !strcmp(current, session) && number >= 0 && (mask & (1U << number));
  free(current); free(command); free(session);
  lx_worker_defer(prior, NULL);
  /* 发送结果不冒充播放状态；发送端随后上报的状态才是 UI 的真值。 */
  int sent = valid ? remote_simple_command_checked((simple_command_t)number, connection) : 0;
  char response[160];
  int n = snprintf(response, sizeof(response), "{\"id\":%llu,\"result\":\"%s\"}",
                   (unsigned long long)id, sent > 0 ? "dispatched" : sent < 0 ? "failed" : "unavailable");
  return packet(4, response, n);
}

static void *serve(void *unused) {
  (void)unused;
  pthread_cleanup_push(cleanup, NULL);
  lx_worker_ready();
  for (;;) {
    lx_worker_checkpoint();
    struct pollfd p = {.fd = listener, .events = POLLIN};
    if (lx_worker_poll(&p, 1, -1) <= 0) continue;
    client = accept4(listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (client < 0) continue;
    struct ucred peer; socklen_t size = sizeof(peer);
    if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &peer, &size) || peer.uid != allowed_uid) {
      close(client); client = -1; continue;
    }
    uint64_t cover = 0;
    while (!send_snapshot(&cover)) {
      struct pollfd waiting[2] = {{.fd = client, .events = POLLIN},
                                  {.fd = listener, .events = POLLIN}};
      if (lx_worker_poll(waiting, 2, 500) < 0) break;
      if (waiting[0].revents & (POLLHUP | POLLERR | POLLNVAL)) break;
      /* 只支持一个客户端；及时拒绝额外连接，不让 backlog 被占满。 */
      if (waiting[1].revents & POLLIN) {
        int extra = accept4(listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (extra >= 0) close(extra);
      }
      if ((waiting[0].revents & POLLIN) && receive_command()) break;
    }
    close(client); client = -1;
  }
  pthread_cleanup_pop(1);
  return NULL;
}

void sps_companion_start(void) {
  int on = 0, uid = -1;
  if (started || !config.cfg) return;
  config_lookup_bool(config.cfg, "android_companion.enabled", &on);
  config_lookup_int(config.cfg, "android_companion.allowed_uid", &uid);
  if (!on) return;
  if (uid < 10000) { warn("Companion: configure the installed App UID (>=10000)."); return; }
  allowed_uid = uid;
  listener = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  memcpy(addr.sun_path + 1, SOCKET_NAME, strlen(SOCKET_NAME));
  if (listener < 0 || bind(listener, (struct sockaddr *)&addr,
      offsetof(struct sockaddr_un, sun_path) + 1 + strlen(SOCKET_NAME)) || listen(listener, 1)) {
    warn("Companion: local listener unavailable: %s", strerror(errno)); cleanup(NULL); return;
  }
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  snprintf(instance, sizeof(instance), "%d-%lld-%ld", getpid(), (long long)t.tv_sec, t.tv_nsec);
  enabled = 1;
  if (lx_worker_create(&thread, NULL, serve, NULL)) {
    enabled = 0; cleanup(NULL); warn("Companion: cannot create bridge worker.");
  } else started = 1;
  if (started && config.android_sync_system_volume && !config.ignore_volume_control) {
    if (lx_worker_create(&volume_thread, NULL, poll_volume, NULL)) {
      warn("Companion: cannot create system volume worker.");
      return;
    }
    volume_enabled = 1;
    if (lx_worker_create(&volume_events_thread, NULL, send_volume_events, NULL))
      warn("Companion: cannot create system volume event worker.");
    else
      volume_events_started = 1;
  }
}

void sps_companion_stop(void) {
  if (!started) return;
  if (volume_events_started) {
    lx_worker_request_stop(volume_events_thread);
    lx_worker_join(volume_events_thread, NULL);
    volume_events_started = 0;
  }
  if (volume_enabled) {
    volume_enabled = 0;
    lx_worker_request_stop(volume_thread);
    lx_worker_join(volume_thread, NULL);
  }
  lx_worker_request_stop(thread); lx_worker_join(thread, NULL);
  started = 0;
  sps_companion_picture(NULL, 0);
  enabled = 0;
}
