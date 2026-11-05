/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Pixelw and contributors
 * See android/LICENSE for the full license text.
 */
#ifndef LX_STOP_TOKEN_H
#define LX_STOP_TOKEN_H
#include <stdatomic.h>
typedef struct {
  int fd[2]; /* 非阻塞唤醒管道：读端、写端；等待者不得排空。 */
  atomic_int requested; /* 当前生命周期的停止标记。 */
} lx_stop_token;
/* 为单个工作线程生命周期创建停止状态和唤醒资源。 */
int lx_stop_init(lx_stop_token *token);
/* 普通线程可重复请求停止；不可从信号处理器调用。 */
int lx_stop_request(lx_stop_token *token);
/* 查询停止状态，不消费管道中的持久可读通知。 */
int lx_stop_requested(const lx_stop_token *token);
/* 所有请求者和等待者结束并 join 后才可销毁。 */
void lx_stop_destroy(lx_stop_token *token);
#endif
