/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Pixelw and contributors
 * See android/LICENSE for the full license text.
 */
/* LX04 公共 NDK AAudio 后端；不使用私有 AudioTrack C++ ABI 或全局音量。 */
#include "audio.h"
#include "common.h"
#include "clock_map.h"
#include "aaudio_diag.h"
#include "worker.h"
#include <aaudio/AAudio.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static AAudioStream *stream;
static uint64_t epoch=1;
static int64_t written;
static int32_t xruns,device_id;
static int64_t last_raw,last_mono,starting_since;
static int configured,started,failed,epoch_notice=1;
static lx_timestamp_state timestamp_state;
static lx_aaudio_snapshot diagnostic={.delay_frames=-1,.timestamp_result=-895,.reason="not_started"};

static int64_t clock_ns(clockid_t id);

/* getState只返回缓存；公开wait接口负责推进Android O客户端状态机。 */
static aaudio_stream_state_t current_state(void){
  aaudio_stream_state_t state=AAUDIO_STREAM_STATE_UNKNOWN;
  if(AAudioStream_waitForStateChange(stream,AAUDIO_STREAM_STATE_UNKNOWN,&state,0)!=AAUDIO_OK)
    return AAUDIO_STREAM_STATE_DISCONNECTED;
  return state;
}

/* 只允许持锁调用，等待异步状态转换；分段等待保留协作停止检查。 */
static int wait_state(aaudio_stream_state_t target,int stopping){
  int64_t begin=clock_ns(CLOCK_MONOTONIC);
  for(;;){
    aaudio_stream_state_t state=current_state();
    diagnostic.state=state;
    if(state==target)return 0;
    if(state==AAUDIO_STREAM_STATE_DISCONNECTED || state==AAUDIO_STREAM_STATE_CLOSED)return -1;
    int64_t now=clock_ns(CLOCK_MONOTONIC);
    if(begin<0 || now<begin || now-begin>=500000000LL || (!stopping && lx_worker_stopping()))return -1;
    aaudio_stream_state_t next=state;
    aaudio_result_t result=AAudioStream_waitForStateChange(stream,state,&next,20000000LL);
    if(result!=AAUDIO_OK && result!=AAUDIO_ERROR_TIMEOUT)return -1;
  }
}
static int64_t clock_ns(clockid_t id){struct timespec t;if(clock_gettime(id,&t))return -1;return (int64_t)t.tv_sec*1000000000LL+t.tv_nsec;}
static void invalidate(void){epoch_notice=1;last_raw=last_mono=0;++epoch;memset(&timestamp_state,0,sizeof(timestamp_state));failed=1;}
static int close_stream(void){
  int result=0;
  if(stream){
    if(started && (AAudioStream_requestStop(stream)!=AAUDIO_OK ||
                   wait_state(AAUDIO_STREAM_STATE_STOPPED,1)))result=-1;
    if(AAudioStream_close(stream)!=AAUDIO_OK)result=-1;
    else ++diagnostic.closes;
  }
  stream=NULL;started=0;written=0;invalidate();
  diagnostic.accepted=0;diagnostic.delay_frames=-1;diagnostic.state=AAUDIO_STREAM_STATE_CLOSED;if(result)configured=0;
  if(result)diagnostic.close_result=result;return result;
}
static int open_stream(void){
  AAudioStreamBuilder *builder=NULL;
  if(!configured)return -1;
  if(stream && !failed)return 0;
  if(stream && close_stream())return -1;
  if(AAudio_createStreamBuilder(&builder)!=AAUDIO_OK)return -1;
  AAudioStreamBuilder_setDirection(builder,AAUDIO_DIRECTION_OUTPUT);
  AAudioStreamBuilder_setSharingMode(builder,AAUDIO_SHARING_MODE_SHARED);
  AAudioStreamBuilder_setPerformanceMode(builder,AAUDIO_PERFORMANCE_MODE_NONE);
  /* 为网络播放器预留更深队列，增加共享输出的调度余量。 */
  AAudioStreamBuilder_setBufferCapacityInFrames(builder,12000);
  AAudioStreamBuilder_setSampleRate(builder,48000);
  AAudioStreamBuilder_setChannelCount(builder,2);
  AAudioStreamBuilder_setFormat(builder,AAUDIO_FORMAT_PCM_I16);
  aaudio_result_t result=AAudioStreamBuilder_openStream(builder,&stream);
  if(stream)++diagnostic.opens;
  aaudio_result_t deleted=AAudioStreamBuilder_delete(builder);
  if(result!=AAUDIO_OK || deleted!=AAUDIO_OK || !stream){if(stream)close_stream();return -1;}
  if(AAudioStream_getSampleRate(stream)!=48000 || AAudioStream_getChannelCount(stream)!=2 ||
     AAudioStream_getFormat(stream)!=AAUDIO_FORMAT_PCM_I16 ||
     AAudioStream_getSharingMode(stream)!=AAUDIO_SHARING_MODE_SHARED){close_stream();return -1;}
  diagnostic.rate=AAudioStream_getSampleRate(stream);diagnostic.channels=AAudioStream_getChannelCount(stream);
  diagnostic.format=AAudioStream_getFormat(stream);diagnostic.sharing=AAudioStream_getSharingMode(stream);
  diagnostic.performance=AAudioStream_getPerformanceMode(stream);diagnostic.burst=AAudioStream_getFramesPerBurst(stream);
  diagnostic.buffer=AAudioStream_getBufferSizeInFrames(stream);diagnostic.capacity=AAudioStream_getBufferCapacityInFrames(stream);
  ++epoch;epoch_notice=1;written=0;started=0;failed=0;last_raw=last_mono=0;xruns=AAudioStream_getXRunCount(stream);device_id=AAudioStream_getDeviceId(stream);
  diagnostic.device=device_id;diagnostic.state=current_state();
  diagnostic.accepted=0;diagnostic.delay_frames=-1;diagnostic.reason="not_started";diagnostic.timestamp_result=-895;
  diagnostic.position=diagnostic.point_ns=diagnostic.now_ns=0;
  diagnostic.raw_before_ns=diagnostic.raw_after_ns=diagnostic.raw_now_ns=0;
  memset(&timestamp_state,0,sizeof(timestamp_state));
  if(xruns<0){close_stream();return -1;}
  return 0;
}
static int init(int argc,char **argv){
  (void)argc;(void)argv;
  /* 首个媒体帧到达前尚未协商格式，属于健康待机，不能触发播放器反复flush。 */
  pthread_mutex_lock(&lock);configured=0;failed=0;pthread_mutex_unlock(&lock);
  parse_audio_options("aaudio",1U<<SPS_FORMAT_S16_LE,1U<<SPS_RATE_48000,1U<<2);
  return 0;
}
static void deinit(void){pthread_mutex_lock(&lock);close_stream();configured=0;pthread_mutex_unlock(&lock);}
static int accepts(unsigned channels,unsigned rate,unsigned format){return channels==2 && rate==48000 && format==SPS_FORMAT_S16_LE;}
static int check_configuration(unsigned channels,unsigned rate,unsigned format){return accepts(channels,rate,format)?0:-1;}
static int32_t get_configuration(unsigned channels,unsigned rate,unsigned format){return search_for_suitable_configuration(channels,rate,format,check_configuration);}
static int configure(int32_t encoded,char **map){
  if(map)*map=NULL;
  if(!accepts(CHANNELS_FROM_ENCODED_FORMAT(encoded),RATE_FROM_ENCODED_FORMAT(encoded),FORMAT_FROM_ENCODED_FORMAT(encoded)))return -1;
  pthread_mutex_lock(&lock);int result=close_stream();configured=result==0;failed=result!=0;pthread_mutex_unlock(&lock);return result;
}
static int prepare(void){pthread_mutex_lock(&lock);int result=configured?open_stream():0;pthread_mutex_unlock(&lock);return result;}
/* 在启动前用零PCM预填实际缓冲；API27 legacy的正超时只表示无限阻塞，故统一非阻塞推进。 */
static int prime_stream(void){
  int32_t target=diagnostic.buffer;
  if(target<=0 || target>diagnostic.capacity || target>48000){diagnostic.reason="prime_parameters";return -1;}
  int16_t zeros[480]={0};int64_t begin=clock_ns(CLOCK_MONOTONIC);
  int primed=0;
  while(primed<target){
    int64_t now=clock_ns(CLOCK_MONOTONIC);
    if(begin<0 || now<begin || now-begin>500000000LL || lx_worker_stopping()){diagnostic.reason="prime_timeout";return -1;}
    int chunk=target-primed;if(chunk>240)chunk=240;
    aaudio_result_t count=AAudioStream_write(stream,zeros,chunk,0);
    diagnostic.write_result=count;diagnostic.write_requested=chunk;
    if(count<=0 || count>chunk){diagnostic.reason="prime_incomplete";return -1;}
    primed+=count;written+=count;
  }
  return 0;
}
static int play(void *buffer,int frames,int sample_type,uint32_t timestamp,uint64_t playtime){
  (void)sample_type;(void)timestamp;(void)playtime;
  if(frames<0 || (frames && !buffer))return -1;
  int old;lx_worker_defer(LX_STOP_DEFERRED,&old);
  pthread_mutex_lock(&lock);
  int result=-1;
  if(open_stream())goto done;
  if(!started){
    if(prime_stream()){invalidate();goto done;}
    if(AAudioStream_requestStart(stream)!=AAUDIO_OK){diagnostic.reason="start_error";invalidate();goto done;}
    started=1;starting_since=clock_ns(CLOCK_MONOTONIC);
    if(starting_since<0){invalidate();goto done;}
    /* Legacy输出可能要先收到PCM才会STARTED；跨短写推进，不能先等待再写。 */
  }
  int offset=0;
  int64_t begin=clock_ns(CLOCK_MONOTONIC);
  if(begin<0)goto fault;
  while(offset<frames){
    if(lx_worker_stopping())goto fault;
    int64_t now=clock_ns(CLOCK_MONOTONIC);
    if(now<begin || now-begin>500000000LL)goto fault;
    int chunk=frames-offset;if(chunk>240)chunk=240;
    aaudio_result_t count=AAudioStream_write(stream,(const int16_t *)buffer+offset*2,chunk,0);
    diagnostic.write_result=count;diagnostic.write_requested=chunk;
    if(count<0){diagnostic.reason="write_error";invalidate();goto done;}
    if(count>chunk || written>INT64_MAX-count)goto fault;
    if(count<chunk)++diagnostic.short_writes;
    if(!count){++diagnostic.zero_writes;struct timespec pause={.tv_nsec=1000000};nanosleep(&pause,NULL);}
    offset+=count;written+=count;
    int32_t current_xruns=AAudioStream_getXRunCount(stream);
    diagnostic.xruns=current_xruns;diagnostic.device=AAudioStream_getDeviceId(stream);
    if(current_xruns<0 || current_xruns!=xruns){diagnostic.reason="xrun";invalidate();goto done;}
    if(diagnostic.device!=device_id){diagnostic.reason="route_changed";invalidate();goto done;}
    aaudio_stream_state_t state=current_state();
    diagnostic.state=state;
    if(state!=AAUDIO_STREAM_STATE_STARTED && state!=AAUDIO_STREAM_STATE_STARTING)goto fault;
    if(state==AAUDIO_STREAM_STATE_STARTING && clock_ns(CLOCK_MONOTONIC)-starting_since>500000000LL){diagnostic.reason="start_timeout";invalidate();goto done;}
  }
  result=0;goto done;
fault:diagnostic.reason="write_discontinuity";invalidate();
done:pthread_mutex_unlock(&lock);lx_worker_defer(old,NULL);return result;
}
static int stats(uint64_t *raw_time,uint64_t *mono_time,uint64_t *delay,uint64_t *sent){
  /* 错误输出全部归零，诊断则保留原始采样及拒绝原因。 */
  if(raw_time)*raw_time=0;if(mono_time)*mono_time=0;if(delay)*delay=0;if(sent)*sent=0;
  pthread_mutex_lock(&lock);int result=-1;
  diagnostic.accepted=0;diagnostic.delay_frames=-1;
  diagnostic.position=diagnostic.point_ns=diagnostic.now_ns=0;
  diagnostic.raw_before_ns=diagnostic.raw_after_ns=diagnostic.raw_now_ns=0;
  diagnostic.timestamp_result=-895;diagnostic.reason="not_started";
  if(!stream || !started || failed)goto done;
  diagnostic.xruns=AAudioStream_getXRunCount(stream);
  diagnostic.device=AAudioStream_getDeviceId(stream);
  diagnostic.state=current_state();
  if(diagnostic.xruns!=xruns){diagnostic.reason="xrun";goto fault;}
  if(diagnostic.device!=device_id){diagnostic.reason="route_changed";goto fault;}
  if(diagnostic.state==AAUDIO_STREAM_STATE_STARTING){
    int64_t current=clock_ns(CLOCK_MONOTONIC);
    if(current>=starting_since && current-starting_since<=500000000LL){diagnostic.reason="start_pending";goto done;}
  }
  if(diagnostic.state!=AAUDIO_STREAM_STATE_STARTED){diagnostic.reason="stream_state";goto fault;}
  int64_t position=0,point=0;
  diagnostic.timestamp_result=AAudioStream_getTimestamp(stream,CLOCK_MONOTONIC,&position,&point);
  diagnostic.position=position;diagnostic.point_ns=point;
  int64_t before=clock_ns(CLOCK_MONOTONIC_RAW),now=clock_ns(CLOCK_MONOTONIC),after=clock_ns(CLOCK_MONOTONIC_RAW);
  diagnostic.now_ns=now;diagnostic.raw_before_ns=before;diagnostic.raw_after_ns=after;
  lx_clock_pair pair;int64_t queue=0,raw=0;
  if(lx_clock_pair_make(before,now,after,&pair)){diagnostic.reason="clock_pair";goto fault;}
  /* 调度抢占只会降低本次相关采样的精度；拒绝该点，不因此销毁健康音频流。 */
  if(pair.uncertainty_ns>1000000){diagnostic.reason="clock_sample_uncertain";goto done;}
  if(lx_mono_to_raw(&pair,now,after,250000000,&raw)){diagnostic.reason="clock_pair";goto fault;}
  diagnostic.raw_now_ns=raw;
  if(last_mono){
    int64_t raw_step=raw-last_raw,mono_step=now-last_mono;
    if(raw_step<0 || mono_step<0 || mono_step>1000000000LL || raw_step-mono_step>2000000 || mono_step-raw_step>2000000){diagnostic.reason="clock_discontinuity";goto fault;}
  }
  last_raw=raw;last_mono=now;
  if(diagnostic.timestamp_result!=AAUDIO_OK){
    if(timestamp_state.have_point)++epoch;timestamp_state.have_point=0;epoch_notice=1;
    diagnostic.reason="timestamp_unavailable";goto done;
  }
  if(lx_delay_frames(&timestamp_state,epoch,written,position,point,now,48000,250000000,&queue)){
    /* 拒绝当前呈现点并重新建立基线；不外推旧样本。 */
    if(timestamp_state.have_point)++epoch;
    timestamp_state.have_point=0;epoch_notice=1;diagnostic.reason="timestamp_invalid";goto done;
  }
  if(epoch_notice){epoch_notice=0;diagnostic.reason="epoch_warmup";goto done;}
  if(raw_time)*raw_time=(uint64_t)raw;
  if(mono_time)*mono_time=(uint64_t)now;
  if(delay)*delay=(uint64_t)queue;
  if(sent)*sent=(uint64_t)written;
  diagnostic.delay_frames=queue;diagnostic.accepted=1;diagnostic.reason="accepted";
  result=0;goto done;
fault:invalidate();
done:diagnostic.epoch=epoch;diagnostic.written=written;diagnostic.failed=failed;
  pthread_mutex_unlock(&lock);return result;
}

void lx_aaudio_diagnostics(lx_aaudio_snapshot *out){
  if(!out)return;
  pthread_mutex_lock(&lock);*out=diagnostic;
  out->epoch=epoch;out->written=written;out->failed=failed;
  pthread_mutex_unlock(&lock);
}
static int delay(long *value){uint64_t raw,mono,queue,sent;if(!value || stats(&raw,&mono,&queue,&sent) || queue>LONG_MAX)return -1;*value=(long)queue;return 0;}
static void stop(void){pthread_mutex_lock(&lock);close_stream();pthread_mutex_unlock(&lock);}
static void flush(void){
  /* 每次 flush 关闭旧流，下一次 play/prepare 使用全新帧原点，避免混用 AAudio 的累计帧数。 */
  pthread_mutex_lock(&lock);failed=close_stream()!=0;pthread_mutex_unlock(&lock);
}
static int is_running(void){
  pthread_mutex_lock(&lock);int result=-1;
  /* 上游在取首帧及配置重采样器之前检查此状态；健康的未开流状态必须可继续。 */
  if(!failed && !stream)result=0;
  if(stream && !failed){aaudio_stream_state_t state=current_state();
    diagnostic.state=state;
    /* 状态检查也必须保留真实故障，避免flush重开后把采样间的XRUN误报成零。 */
    diagnostic.xruns=AAudioStream_getXRunCount(stream);diagnostic.device=AAudioStream_getDeviceId(stream);
    if(diagnostic.xruns!=xruns){diagnostic.reason="xrun";invalidate();}
    else if(diagnostic.device!=device_id){diagnostic.reason="route_changed";invalidate();}
    else if(state==AAUDIO_STREAM_STATE_OPEN || (state==AAUDIO_STREAM_STATE_STARTING && clock_ns(CLOCK_MONOTONIC)>=starting_since && clock_ns(CLOCK_MONOTONIC)-starting_since<=500000000LL) || state==AAUDIO_STREAM_STATE_STARTED)result=0;}
  pthread_mutex_unlock(&lock);return result;
}
/* 真实 SPS 后端导出；软件音量/静音由上游处理，无系统音量副作用。 */
audio_output audio_aaudio={.name="aaudio",.init=init,.deinit=deinit,.prepare=prepare,
  .get_configuration=get_configuration,.configure=configure,.play=play,.stop=stop,
  .is_running=is_running,.flush=flush,.delay=delay,.stats=stats,.volume=NULL,.mute=NULL,.parameters=NULL,.help=NULL};
