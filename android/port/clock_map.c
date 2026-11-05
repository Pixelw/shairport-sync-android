/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Pixelw and contributors
 * See android/LICENSE for the full license text.
 */
#include "clock_map.h"
#include <limits.h>
#include <stddef.h>

int lx_clock_pair_make(int64_t before, int64_t mono, int64_t after,
                       lx_clock_pair *out) {
  if (!out || before < 0 || mono < 0 || after < before)
    return -1;
  int64_t span = after - before;
  out->raw_mid_ns = before + span / 2;
  out->mono_ns = mono;
  out->uncertainty_ns = span / 2 + span % 2;
  return 0;
}

int lx_mono_to_raw(const lx_clock_pair *p, int64_t mono, int64_t now,
                   int64_t limit, int64_t *out) {
  if (!p || !out || limit < 0 || mono < 0 || now < 0 || p->mono_ns < 0 ||
      p->raw_mid_ns < 0 || p->uncertainty_ns < 0 || p->uncertainty_ns > limit)
    return -1;
  int64_t age = now - p->raw_mid_ns;
  int64_t delta = mono - p->mono_ns;
  if (age < 0 || age > limit || delta < -limit || delta > limit)
    return -1;
  if ((delta > 0 && p->raw_mid_ns > INT64_MAX - delta) ||
      (delta < 0 && p->raw_mid_ns < -delta))
    return -1;
  *out = p->raw_mid_ns + delta;
  return 0;
}

int lx_delay_frames(lx_timestamp_state *s, uint64_t epoch, int64_t w,
                    int64_t p, int64_t t, int64_t now, int32_t rate,
                    int64_t max_age, int64_t *delay) {
  if (!s || !delay || w < 0 || p < 0 || t < 0 || now < 0 || rate <= 0 ||
      rate > 384000 || max_age <= 0 || max_age > 1000000000LL)
    return -1;
  if (s->epoch != epoch) {
    s->epoch = epoch;
    s->have_point = 0;
  }
  /* Conservative probe policy: reject future points instead of pretending
   * they are already presented. A production backend may model them. */
  if (t > now || now - t > max_age || p > w)
    return -1;
  if (s->have_point && (p < s->position || t < s->time_ns ||
                        (p == s->position && t != s->time_ns) ||
                        (p != s->position && t == s->time_ns)))
    return -1;
  int64_t advance = (now - t) * rate / 1000000000LL;
  /* Exhausted queue / underrun is unknown, not a confident zero delay. */
  if (advance > w - p)
    return -1;
  *delay = w - p - advance;
  s->have_point = 1;
  s->position = p;
  s->time_ns = t;
  return 0;
}
