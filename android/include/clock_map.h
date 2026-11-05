/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Pixelw and contributors
 * See android/LICENSE for the full license text.
 */
#ifndef LX_CLOCK_MAP_H
#define LX_CLOCK_MAP_H
#include <stdint.h>

/* 单次 RAW/MONOTONIC 相关采样；休眠或音频世代变化后不可复用。 */
typedef struct {
  int64_t raw_mid_ns; /* RAW 前后样本的中点，单位纳秒。 */
  int64_t mono_ns; /* 两次 RAW 采样之间的 MONOTONIC 样本。 */
  int64_t uncertainty_ns; /* RAW 采样半宽，不包含长期漂移误差。 */
} lx_clock_pair;

/* 校验采样顺序并计算中点，拒绝算术溢出。 */
int lx_clock_pair_make(int64_t raw_before, int64_t mono, int64_t raw_after,
                       lx_clock_pair *out);
/* 只换算距离相关样本足够近的时间；过期或超距返回 -1。 */
int lx_mono_to_raw(const lx_clock_pair *pair, int64_t mono_measurement,
                   int64_t now_raw, int64_t max_distance_ns, int64_t *raw);

typedef struct {
  uint64_t epoch; /* 当前音频流世代。 */
  int have_point; /* 是否已有可信呈现时间戳。 */
  int64_t position; /* 上次可信呈现位置，单位帧。 */
  int64_t time_ns; /* 上次可信呈现时刻，MONOTONIC 纳秒。 */
} lx_timestamp_state;

/* 以 MONOTONIC 时间估算队列帧数，无效时间戳必须返回失败。
 * 调用方负责世代重置、XRUN/路由处理以及成功写入帧数的统一原点。 */
int lx_delay_frames(lx_timestamp_state *state, uint64_t epoch, int64_t written,
                    int64_t position, int64_t time_ns, int64_t now_ns,
                    int32_t rate, int64_t max_age_ns, int64_t *delay);
#endif
