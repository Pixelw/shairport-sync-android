/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Pixelw and contributors
 * See android/LICENSE for the full license text.
 */
#ifndef LX_LOG_H
#define LX_LOG_H

/* 日志级别独立于 Android 头文件，主机构建使用相同接口。 */
typedef enum {
  LX_LOG_ERROR, /* 错误及致命错误；不替代调用者的退出流程。 */
  LX_LOG_WARN,  /* 可恢复的警告。 */
  LX_LOG_INFO,  /* 上游常规运行信息及播放统计。 */
  LX_LOG_DEBUG /* 由上游详细程度控制的调试信息。 */
} lx_log_level;

/* 在线程启动前选择日志目标并移除 --log-output= 参数；失败返回 -1。 */
int lx_log_init(const char *tag, int *argc, char **argv);
/* 输出已格式化文本，保留 errno；不将文本解释为格式字符串。 */
void lx_log_write(lx_log_level level, const char *message);
/* 格式化后输出一条日志；最多保留 4095 字节，保留 errno。 */
void lx_log_printf(lx_log_level level, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

#endif
