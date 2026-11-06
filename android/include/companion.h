/* SPDX-License-Identifier: MIT */
#ifndef SPS_COMPANION_H
#define SPS_COMPANION_H
#include <stddef.h>
#include <stdint.h>
/* 按配置启动本机桥接；失败只记录日志，不影响接收器。 */
void sps_companion_start(void);
/* 协作停止、唤醒并回收桥接线程。 */
void sps_companion_stop(void);
/* 替换有界封面快照；返回内容是否更新。 */
int sps_companion_picture(const char *data, size_t length);
/* 音频线程尝试发布计时快照；锁忙时直接跳过，不等待。 */
void sps_companion_timing(int connection, uint64_t frames, uint32_t timestamp);
/* 返回已发布的帧计数，避免 ARM32 上跨线程撕裂读取。 */
uint64_t sps_companion_frames(int connection);
#endif
