/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Pixelw and contributors
 * See android/LICENSE for the full license text.
 */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "worker.h"
#include "stop_token.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* 生命周期由创建者持有，只有 join 后才销毁；请求者在注册锁内使用。 */
typedef struct worker {
  pthread_t thread;
  lx_stop_token stop;
  void *(*entry)(void *);
  void *arg;
  pthread_cond_t *waiting_condition;
  int joined;
  struct worker *next;
} worker;
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static worker *registry;
static _Thread_local worker *current;
static _Thread_local int deferred;
static _Thread_local int exiting;

static worker *find(pthread_t thread) {
  for (worker *p=registry;p;p=p->next) if(pthread_equal(p->thread,thread))return p;
  return NULL;
}
static void finished(void *arg) {
  worker *p=arg;
  pthread_mutex_lock(&registry_lock);
  p->waiting_condition=NULL;
  pthread_mutex_unlock(&registry_lock);
  current=NULL;
}
static void *start(void *arg) {
  worker *p=arg;
  /* 创建者持注册锁直到 pthread_t 与注册记录均已发布。 */
  pthread_mutex_lock(&registry_lock);
  pthread_mutex_unlock(&registry_lock);
  current=p;deferred=LX_STOP_DEFERRED;exiting=0;
  void *result;
  pthread_cleanup_push(finished,p);
  result=p->entry(p->arg);
  pthread_cleanup_pop(1);
  return result;
}
int lx_worker_create(pthread_t *thread,const pthread_attr_t *attr,void *(*entry)(void *),void *arg) {
  int state=PTHREAD_CREATE_JOINABLE;
  if(attr && pthread_attr_getdetachstate(attr,&state))return EINVAL;
  if(state!=PTHREAD_CREATE_JOINABLE)return EINVAL;
  worker *p=calloc(1,sizeof(*p));
  if(!p)return ENOMEM;
  if(lx_stop_init(&p->stop)){int e=errno;free(p);return e;}
  p->entry=entry;p->arg=arg;
  pthread_mutex_lock(&registry_lock);
  int result=pthread_create(thread,attr,start,p);
  if(!result){p->thread=*thread;p->next=registry;registry=p;}
  pthread_mutex_unlock(&registry_lock);
  if(result){lx_stop_destroy(&p->stop);free(p);}
  return result;
}
int lx_worker_request_stop(pthread_t thread) {
  pthread_mutex_lock(&registry_lock);
  worker *p=find(thread);
  int result=ESRCH;
  if(p){
    result=lx_stop_request(&p->stop) ? errno : 0;
    if(p->waiting_condition)pthread_cond_broadcast(p->waiting_condition);
  }
  pthread_mutex_unlock(&registry_lock);
  return result;
}
int lx_worker_join(pthread_t thread,void **value) {
  if(pthread_equal(thread,pthread_self()))return EDEADLK;
  pthread_mutex_lock(&registry_lock);
  worker *p=find(thread);
  if(!p || p->joined){pthread_mutex_unlock(&registry_lock);return p ? EINVAL:ESRCH;}
  p->joined=1;
  pthread_mutex_unlock(&registry_lock);
  int result=pthread_join(thread,value);
  pthread_mutex_lock(&registry_lock);
  if(!result){worker **at=&registry;while(*at!=p)at=&(*at)->next;*at=p->next;}
  else p->joined=0;
  pthread_mutex_unlock(&registry_lock);
  if(!result){lx_stop_destroy(&p->stop);free(p);}
  return result;
}
void lx_worker_ready(void){deferred=LX_STOP_ENABLED;lx_worker_checkpoint();}
int lx_worker_stopping(void){return current && lx_stop_requested(&current->stop);}
void lx_worker_checkpoint(void){
  if(!deferred && !exiting && lx_worker_stopping()){
    exiting=1;
    pthread_exit(NULL);
  }
}
int lx_worker_defer(int value,int *old){
  if(value!=LX_STOP_ENABLED && value!=LX_STOP_DEFERRED)return EINVAL;
  if(old)*old=deferred;
  deferred=value;
  if(!deferred)lx_worker_checkpoint();
  return 0;
}
unsigned lx_worker_count(void){
  unsigned count=0;pthread_mutex_lock(&registry_lock);
  for(worker *p=registry;p;p=p->next)++count;
  pthread_mutex_unlock(&registry_lock);return count;
}
static int64_t monotonic_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
int lx_worker_poll(struct pollfd *fds,nfds_t count,int timeout){
  lx_worker_checkpoint();
  if(!current || deferred || exiting)return poll(fds,count,timeout);
  if(count>FD_SETSIZE){errno=EINVAL;return -1;}
  struct pollfd all[FD_SETSIZE+1];
  if(count)memcpy(all,fds,count*sizeof(*fds));
  all[count]=(struct pollfd){.fd=current->stop.fd[0],.events=POLLIN};
  int result=poll(all,count+1,timeout);
  if(result>0 && all[count].revents){lx_worker_checkpoint();--result;}
  if(count)memcpy(fds,all,count*sizeof(*fds));
  return result;
}
int lx_worker_select(int n,fd_set *reads,fd_set *writes,fd_set *errors,struct timeval *timeout){
  if(n<0 || n>FD_SETSIZE || (timeout && (timeout->tv_sec<0 || timeout->tv_usec<0 || timeout->tv_usec>=1000000))){errno=EINVAL;return -1;}
  struct pollfd fds[FD_SETSIZE];nfds_t count=0;
  for(int i=0;i<n;++i){short events=0;
    if(reads && FD_ISSET(i,reads))events|=POLLIN;
    if(writes && FD_ISSET(i,writes))events|=POLLOUT;
    if(errors && FD_ISSET(i,errors))events|=POLLPRI;
    if(events)fds[count++]=(struct pollfd){.fd=i,.events=events};
  }
  int64_t ms=timeout ? (int64_t)timeout->tv_sec*1000+(timeout->tv_usec+999)/1000:-1;
  int result=lx_worker_poll(fds,count,ms>INT_MAX?INT_MAX:(int)ms);
  if(result<0)return result;
  if(reads)FD_ZERO(reads);if(writes)FD_ZERO(writes);if(errors)FD_ZERO(errors);
  result=0;
  for(nfds_t i=0;i<count;++i){short r=fds[i].revents;
    if(r&POLLNVAL){errno=EBADF;return -1;}
    if(reads && (fds[i].events&POLLIN) && (r&(POLLIN|POLLHUP|POLLERR))){FD_SET(fds[i].fd,reads);++result;}
    if(writes && (fds[i].events&POLLOUT) && (r&(POLLOUT|POLLERR))){FD_SET(fds[i].fd,writes);++result;}
    if(errors && (fds[i].events&POLLPRI) && (r&POLLPRI)){FD_SET(fds[i].fd,errors);++result;}
  }
  return result;
}
static int await(int fd,short events){
  struct pollfd p={.fd=fd,.events=events};
  int result;
  do{result=lx_worker_poll(&p,1,-1);}while(result<0 && errno==EINTR);
  return result;
}
static int nonblock(int fd);
ssize_t lx_worker_recvfrom(int fd,void *buf,size_t len,int flags,struct sockaddr *addr,socklen_t *size){
  if(nonblock(fd))return -1;
  for(;;){
    lx_worker_checkpoint();
    ssize_t n=recvfrom(fd,buf,len,flags|MSG_DONTWAIT,addr,size);
    if(n>=0 || (errno!=EAGAIN && errno!=EWOULDBLOCK))return n;
    if(flags&MSG_DONTWAIT)return n;
    /* 上游 socket 超时也必须保留；poll 的停止管道始终参与等待。 */
    struct timeval tv={0};socklen_t tvsize=sizeof(tv);
    (void)getsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,&tvsize);
    int64_t ms=(int64_t)tv.tv_sec*1000+(tv.tv_usec+999)/1000;
    struct pollfd p={.fd=fd,.events=POLLIN};
    int r=lx_worker_poll(&p,1,ms? (ms>INT_MAX?INT_MAX:(int)ms):-1);
    if(r<=0){if(!r)errno=EAGAIN;return -1;}
  }
}
ssize_t lx_worker_recv(int fd,void *buf,size_t len,int flags){return lx_worker_recvfrom(fd,buf,len,flags,NULL,NULL);}
static int nonblock(int fd){int flags=fcntl(fd,F_GETFL);if(flags<0)return -1;return fcntl(fd,F_SETFL,flags|O_NONBLOCK);}
ssize_t lx_worker_read(int fd,void *buf,size_t len){
  if(!len)return 0;
  /* 网络 read 用 recvfrom 路径，保留 SO_RCVTIMEO 与调用方非阻塞语义。 */
  int type;socklen_t size=sizeof(type);
  if(!getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&size))return lx_worker_recv(fd,buf,len,0);
  if(nonblock(fd))return -1;
  for(;;){lx_worker_checkpoint();ssize_t n=read(fd,buf,len);if(n>=0 || (errno!=EAGAIN && errno!=EWOULDBLOCK))return n;if(await(fd,POLLIN)<0)return -1;}
}
ssize_t lx_worker_write(int fd,const void *buf,size_t len){
  int type;socklen_t size=sizeof(type);
  if(!getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&size)){
    if(nonblock(fd))return -1;
    for(;;){
      lx_worker_checkpoint();
      ssize_t n=send(fd,buf,len,MSG_DONTWAIT);
      if(n>=0 || (errno!=EAGAIN && errno!=EWOULDBLOCK))return n;
      struct timeval tv={0};size=sizeof(tv);(void)getsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,&size);
      int64_t ms=(int64_t)tv.tv_sec*1000+(tv.tv_usec+999)/1000;
      struct pollfd p={.fd=fd,.events=POLLOUT};
      int r=lx_worker_poll(&p,1,ms?(ms>INT_MAX?INT_MAX:(int)ms):-1);
      if(r<=0){if(!r)errno=EAGAIN;return -1;}
    }
  }
  if(nonblock(fd))return -1;
  for(;;){lx_worker_checkpoint();ssize_t n=write(fd,buf,len);if(n>=0 || (errno!=EAGAIN && errno!=EWOULDBLOCK))return n;if(await(fd,POLLOUT)<0)return -1;}
}
int lx_worker_accept(int fd,struct sockaddr *addr,socklen_t *size){
  if(nonblock(fd))return -1;
  for(;;){lx_worker_checkpoint();int n=accept(fd,addr,size);if(n>=0){int flags=fcntl(n,F_GETFL);if(flags>=0)fcntl(n,F_SETFL,flags&~O_NONBLOCK);return n;}if(errno!=EAGAIN && errno!=EWOULDBLOCK)return n;if(await(fd,POLLIN)<0)return -1;}
}
static void clear_condition(void *unused){
  (void)unused;if(!current)return;
  pthread_mutex_lock(&registry_lock);current->waiting_condition=NULL;pthread_mutex_unlock(&registry_lock);
}
int lx_worker_cond_timedwait(pthread_cond_t *cond,pthread_mutex_t *mutex,const struct timespec *end){
  if(!current || exiting)return end?pthread_cond_timedwait(cond,mutex,end):pthread_cond_wait(cond,mutex);
  int result=0;
  pthread_mutex_lock(&registry_lock);current->waiting_condition=cond;pthread_mutex_unlock(&registry_lock);
  pthread_cleanup_push(clear_condition,NULL);
  for(;;){
    lx_worker_checkpoint();
    struct timespec until;clock_gettime(CLOCK_REALTIME,&until);
    until.tv_nsec+=20000000;if(until.tv_nsec>=1000000000){++until.tv_sec;until.tv_nsec-=1000000000;}
    int final=0;
    if(end && (end->tv_sec<until.tv_sec || (end->tv_sec==until.tv_sec && end->tv_nsec<=until.tv_nsec))){until=*end;final=1;}
    result=pthread_cond_timedwait(cond,mutex,&until);
    lx_worker_checkpoint();
    if(result!=ETIMEDOUT || final)break;
  }
  pthread_cleanup_pop(1);
  return result;
}
int lx_worker_cond_timedwait_relative(pthread_cond_t *cond,pthread_mutex_t *mutex,const struct timespec *relative){
  struct timespec end;clock_gettime(CLOCK_REALTIME,&end);end.tv_sec+=relative->tv_sec;end.tv_nsec+=relative->tv_nsec;
  if(end.tv_nsec>=1000000000){++end.tv_sec;end.tv_nsec-=1000000000;}
  return lx_worker_cond_timedwait(cond,mutex,&end);
}
int lx_worker_cond_wait(pthread_cond_t *cond,pthread_mutex_t *mutex){return lx_worker_cond_timedwait(cond,mutex,NULL);}
int lx_worker_nanosleep(const struct timespec *duration,struct timespec *remaining){
  if(duration->tv_sec<0 || duration->tv_nsec<0 || duration->tv_nsec>=1000000000){errno=EINVAL;return -1;}
  int64_t ms=(int64_t)duration->tv_sec*1000+(duration->tv_nsec+999999)/1000000;
  int64_t start_ms=monotonic_ms();
  int result=lx_worker_poll(NULL,0,ms>INT_MAX?INT_MAX:(int)ms);
  if(remaining){int64_t rem=ms-(monotonic_ms()-start_ms);if(rem<0)rem=0;remaining->tv_sec=rem/1000;remaining->tv_nsec=(rem%1000)*1000000;}
  return result<0?-1:0;
}
int lx_worker_usleep(useconds_t microseconds){struct timespec duration={.tv_sec=microseconds/1000000,.tv_nsec=(microseconds%1000000)*1000};return lx_worker_nanosleep(&duration,NULL);}
