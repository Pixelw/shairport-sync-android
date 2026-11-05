/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Pixelw and contributors
 * See android/LICENSE for the full license text.
 */
#ifndef LX_WORKER_H
#define LX_WORKER_H
#include <pthread.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
/* 停止只在明确检查点执行；临界区可暂缓，禁止异步终止和空取消桩。 */
#define LX_STOP_ENABLED 0
#define LX_STOP_DEFERRED 1
/* 创建带独立停止管道、生命周期和清理栈的可 join 工作线程。 */
int lx_worker_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
/* 入口安装根清理处理器后启用停止检查；此前请求仅挂起，避免跳过资源清理。 */
void lx_worker_ready(void);
/* 按注册的生命周期请求停止并唤醒等待；不直接释放工作线程资源。 */
int lx_worker_request_stop(pthread_t);
/* 等待工作线程清理完成并回收注册句柄；不得持连接表锁调用。 */
int lx_worker_join(pthread_t, void **);
/* 查询本工作线程停止请求，不执行退出。 */
int lx_worker_stopping(void);
/* 明确检查点：已请求且未暂缓时，经 pthread_exit 运行原有清理栈。 */
void lx_worker_checkpoint(void);
/* 设置协作退出暂缓状态；恢复时处理待执行请求，不支持异步退出。 */
int lx_worker_defer(int, int *);
/* 带停止管道的 poll；所有描述符须由当前生命周期持有。 */
int lx_worker_poll(struct pollfd *, nfds_t, int);
/* 带停止唤醒的 select，保持调用方超时和就绪集合契约。 */
int lx_worker_select(int, fd_set *, fd_set *, fd_set *, struct timeval *);
/* 描述符由本模块设为非阻塞并统一 poll；只有显式 MSG_DONTWAIT 不等待。 */
ssize_t lx_worker_recv(int, void *, size_t, int);
/* 可停止的 UDP 接收，返回源地址。 */
ssize_t lx_worker_recvfrom(int, void *, size_t, int, struct sockaddr *, socklen_t *);
/* 可停止的读取，内部使用非阻塞描述符与 poll。 */
ssize_t lx_worker_read(int, void *, size_t);
/* 可停止的写入，允许短写并交由调用方推进。 */
ssize_t lx_worker_write(int, const void *, size_t);
/* 可停止的 accept，监听描述符转为非阻塞，返回的新连接独立持有。 */
int lx_worker_accept(int, struct sockaddr *, socklen_t *);
/* 条件等待登记到生命周期中，停止请求 broadcast；超时兜底防止丢唤醒。 */
int lx_worker_cond_wait(pthread_cond_t *, pthread_mutex_t *);
/* 带绝对 REALTIME 截止时刻的条件等待；当前上游条件均为默认时钟。 */
int lx_worker_cond_timedwait(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
/* 将 Darwin 上游的相对条件超时转换为统一可停止等待，不依赖私有函数。 */
int lx_worker_cond_timedwait_relative(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
/* 可停止的相对休眠。 */
int lx_worker_nanosleep(const struct timespec *, struct timespec *);
/* 可停止的微秒休眠。 */
int lx_worker_usleep(useconds_t);
/* 查询尚未 join 的注册生命周期数，供主机验收。 */
unsigned lx_worker_count(void);
#endif
