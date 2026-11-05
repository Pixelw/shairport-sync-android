/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Pixelw and contributors
 * See android/LICENSE for the full license text.
 */
/* 主线程拥有退出协调；信号处理器只设置标记并写入 self-pipe。 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include "exit.h"
#ifdef LX_HOST_INTEGRATION
#include "worker.h"
#include <assert.h>
#include <stdio.h>
#endif

/* 退出请求，供主循环检查；信号中只写 sig_atomic_t。 */
volatile sig_atomic_t exit_request_flag;
/* 请求的进程退出码。 */
volatile sig_atomic_t exit_status;
static int wake_fd[2]={-1,-1};
static pthread_t main_thread;
static void close_wakeup(void){
#ifdef LX_HOST_INTEGRATION
  fprintf(stderr,"LX_HOST_WORKERS=%u\n",lx_worker_count());assert(lx_worker_count()==0);
#endif
  for(int i=0;i<2;++i)if(wake_fd[i]>=0)close(wake_fd[i]);
}
/* 在主线程初始化非阻塞 self-pipe，不创建额外控制工作线程。 */
void exit_init(void){
  main_thread=pthread_self();
  if(pipe(wake_fd))exit(EXIT_FAILURE);
  for(int i=0;i<2;++i){
    if(fcntl(wake_fd[i],F_SETFD,FD_CLOEXEC)<0 || fcntl(wake_fd[i],F_SETFL,O_NONBLOCK)<0){close_wakeup();exit(EXIT_FAILURE);}
  }
  atexit(close_wakeup);
}
/* 可从信号处理器调用；没有日志、锁、内存分配或工作线程停止操作。 */
void exit_request(const int requested){
  int saved=errno;
  if(requested)exit_status=requested;
  exit_request_flag=1;
  if(wake_fd[1]>=0){char byte=1;(void)write(wake_fd[1],&byte,1);}
  errno=saved;
}
/* 仅供普通错误路径判断，不能从信号处理器调用。 */
int lx_exit_is_main(void){return pthread_equal(main_thread,pthread_self());}
/* 主线程等待请求后返回退出码；返回 main 前由 atexit 依次停止和 join 工作者。 */
int lx_exit_wait(void){
  struct pollfd fd={.fd=wake_fd[0],.events=POLLIN};
  while(!exit_request_flag){if(poll(&fd,1,-1)<0 && errno!=EINTR){exit_status=EXIT_FAILURE;break;}}
  return (int)exit_status;
}
