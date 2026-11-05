/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Pixelw and contributors
 * See android/LICENSE for the full license text.
 */
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "smi_file.h"
#include "smi_abi.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__ANDROID__)
#include <linux/magic.h>
#include <sys/vfs.h>
#endif

int lx_smi_open_directory(const char *path) {
  int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return -1;
  struct stat st;
  if (fstat(fd, &st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
      (st.st_mode & 0777) != 0700) {
    close(fd); errno = EACCES; return -1;
  }
#if defined(__ANDROID__)
  /* 目标必须使用已授权预建的 tmpfs 私有目录，不静默回退到闪存文件。 */
  struct statfs fs;
  if (fstatfs(fd, &fs) || fs.f_type != TMPFS_MAGIC) {
    close(fd); errno = EACCES; return -1;
  }
#endif
  return fd;
}
static int validate(int fd) {
  struct stat st;
  if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
      (st.st_mode & 0777) != 0600 || st.st_nlink != 1 ||
      st.st_size != (off_t)sizeof(struct shm_structure)) {
    errno = EACCES; return -1;
  }
  return 0;
}
int lx_smi_create(int dir) {
  int fd = openat(dir, LX_SMI_LEAF, O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) return -1;
  if (ftruncate(fd, sizeof(struct shm_structure)) || validate(fd)) {
    int saved = errno;
    close(fd);
    /* Only this just-created object; no startup scavenging. */
    unlinkat(dir, LX_SMI_LEAF, 0);
    errno = saved;
    return -1;
  }
  return fd;
}
int lx_smi_open_reader(int dir) {
  int fd = openat(dir, LX_SMI_LEAF, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) return -1;
  if (validate(fd)) { int saved = errno; close(fd); errno = saved; return -1; }
  return fd;
}
int lx_smi_is_current(int dir, int fd) {
  struct stat opened, named;
  if (validate(fd) || fstat(fd, &opened) ||
      fstatat(dir, LX_SMI_LEAF, &named, AT_SYMLINK_NOFOLLOW)) return 0;
  return S_ISREG(named.st_mode) && opened.st_dev == named.st_dev &&
         opened.st_ino == named.st_ino;
}
int lx_smi_publish(int fd, struct shm_structure *mapped,
                   const struct shm_structure *value) {
  if (value->version != 11 ||
      memcmp(&value->main, &value->secondary, sizeof(value->main))) {
    errno = EINVAL; return -1;
  }
  if (flock(fd, LOCK_EX | LOCK_NB)) return -1;
  /* 先作废再更新：写者意外退出并释放锁时，读方不得接受半成品。 */
  __atomic_store_n(&mapped->version, 0, __ATOMIC_SEQ_CST);
  __sync_synchronize();
  /* 发布检查点：已作废。 */
  memcpy(&mapped->main, &value->main, sizeof(mapped->main));
  /* 发布检查点：主副本已写入。 */
  memcpy(&mapped->secondary, &value->secondary, sizeof(mapped->secondary));
  __sync_synchronize();
  /* 发布检查点：两个副本已写入。 */
  __atomic_store_n(&mapped->version, NQPTP_SHM_STRUCTURES_VERSION, __ATOMIC_RELEASE);
  /* 发布检查点：有效版本已发布。 */
  return flock(fd, LOCK_UN);
}
int lx_smi_snapshot(int fd, const struct shm_structure *mapped,
                    struct shm_structure *value) {
  if (flock(fd, LOCK_SH | LOCK_NB)) return -1;
  __sync_synchronize();
  memcpy(value, mapped, sizeof(*value));
  __sync_synchronize();
  int result = flock(fd, LOCK_UN);
  if (result) return result;
  if (value->version != 11 ||
      memcmp(&value->main, &value->secondary, sizeof(value->main))) {
    errno = EAGAIN; return -1;
  }
  return 0;
}
