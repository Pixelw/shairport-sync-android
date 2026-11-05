/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Pixelw and contributors
 * See android/LICENSE for the full license text.
 */
#ifndef LX_AAUDIO_DIAG_H
#define LX_AAUDIO_DIAG_H
#include <stdint.h>

/* 最近一次后端采样；只含数字，不含媒体、认证状态或系统音量。 */
typedef struct {
  uint64_t epoch; /* 音频流世代。 */
  int64_t written; /* 本世代成功写入帧数。 */
  int64_t position; /* AAudio 原始呈现帧位置。 */
  int64_t point_ns; /* 原始 MONOTONIC 呈现时间。 */
  int64_t now_ns; /* 采样 MONOTONIC 时间。 */
  int64_t raw_before_ns; /* 相关采样之前的 RAW 时间。 */
  int64_t raw_after_ns; /* 相关采样之后的 RAW 时间。 */
  int64_t raw_now_ns; /* 换算后的当前 RAW 时间。 */
  int64_t delay_frames; /* 有效时的排队帧数，否则为负。 */
  int32_t write_result; /* 最近一次写入返回的帧数或错误码。 */
  int32_t write_requested; /* 最近一次写入请求帧数。 */
  int32_t timestamp_result; /* 公共 API 时间戳返回码。 */
  int32_t xruns; /* 当前欠载累计计数。 */
  int32_t device; /* 当前输出设备编号。 */
  int32_t state; /* 当前公共流状态。 */
  int32_t rate; /* 实际采样率。 */
  int32_t channels; /* 实际声道数。 */
  int32_t format; /* 实际样本格式。 */
  int32_t sharing; /* 实际共享模式。 */
  int32_t performance; /* 实际性能模式。 */
  int32_t burst; /* 实际每突发帧数。 */
  int32_t buffer; /* 实际缓冲区帧数。 */
  int32_t capacity; /* 实际缓冲区容量。 */
  int accepted; /* 本次 stats 是否返回有效计时。 */
  int failed; /* 本世代是否已经失效。 */
  int close_result; /* 最近关闭结果；失败不会被新开流掩盖。 */
  uint64_t opens; /* 成功开流累计次数。 */
  uint64_t closes; /* 成功关流累计次数。 */
  uint64_t short_writes; /* 短写累计次数。 */
  uint64_t zero_writes; /* 零写累计次数。 */
  const char *reason; /* 静态拒绝原因名称，调用方不得释放。 */
} lx_aaudio_snapshot;

/* 在后端锁内复制最近一次诊断；本函数不访问驱动、不更新计时。 */
void lx_aaudio_diagnostics(lx_aaudio_snapshot *out);
#endif
