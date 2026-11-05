/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Pixelw and contributors
 * See android/LICENSE for the full license text.
 */
#include "stop_token.h"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

int lx_stop_init(lx_stop_token *t) {
  t->fd[0] = t->fd[1] = -1;
  atomic_init(&t->requested, 0);
  if (pipe(t->fd)) return -1;
  for (int i = 0; i < 2; ++i) {
    if (fcntl(t->fd[i], F_SETFD, FD_CLOEXEC) == -1 ||
        fcntl(t->fd[i], F_SETFL, O_NONBLOCK) == -1) {
      int saved = errno;
      lx_stop_destroy(t);
      errno = saved;
      return -1;
    }
  }
  return 0;
}
int lx_stop_request(lx_stop_token *t) {
  atomic_store_explicit(&t->requested, 1, memory_order_release);
  char byte = 1;
  ssize_t n;
  do { n = write(t->fd[1], &byte, 1); } while (n < 0 && errno == EINTR);
  return (n == 1 || (n < 0 && errno == EAGAIN)) ? 0 : -1;
}
int lx_stop_requested(const lx_stop_token *t) {
  return atomic_load_explicit(&t->requested, memory_order_acquire);
}
void lx_stop_destroy(lx_stop_token *t) {
  for (int i = 0; i < 2; ++i) {
    if (t->fd[i] >= 0) close(t->fd[i]);
    t->fd[i] = -1;
  }
}
