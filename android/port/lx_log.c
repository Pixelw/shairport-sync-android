/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Pixelw and contributors
 * See android/LICENSE for the full license text.
 */
/* LX04 公共日志适配：Android 默认 logcat，主机默认 stderr。 */
#include "lx_log.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifdef __ANDROID__
#include <android/log.h>
static int use_logcat = 1;
#else
static int use_logcat = 0;
#endif
static const char *log_tag = "SPS-main";

int lx_log_init(const char *tag, int *argc, char **argv) {
  int count = 1;
  log_tag = tag;
  /* 仅在启动时调用，目标和 tag 在线程运行期间保持不变。 */
  for (int i = 1; i < *argc; ++i) {
    if (!strcmp(argv[i], "--")) {
      while (i < *argc) argv[count++] = argv[i++];
      break;
    }
    if (!strncmp(argv[i], "--log-output=", 13)) {
      const char *value = argv[i] + 13;
      if (!strcmp(value, "stderr")) use_logcat = 0;
#ifdef __ANDROID__
      else if (!strcmp(value, "logcat")) use_logcat = 1;
#endif
      else {
        lx_log_write(LX_LOG_ERROR, "Invalid --log-output; use logcat (Android only) or stderr.");
        return -1;
      }
    } else if (!strcmp(argv[i], "--log-output")) {
      lx_log_write(LX_LOG_ERROR, "Use --log-output=logcat or --log-output=stderr.");
      return -1;
    } else argv[count++] = argv[i];
  }
  argv[count] = NULL;
  *argc = count;
  return 0;
}

void lx_log_write(lx_log_level level, const char *message) {
  int saved = errno;
#ifdef __ANDROID__
  if (use_logcat) {
    static const int priorities[] = {ANDROID_LOG_ERROR, ANDROID_LOG_WARN,
                                     ANDROID_LOG_INFO, ANDROID_LOG_DEBUG};
    __android_log_write(priorities[level], log_tag, message);
  } else
#else
  (void)level;
  (void)use_logcat;
  (void)log_tag;
#endif
  {
    size_t length = strlen(message);
    /* 单次 stdio 调用保证并发日志不在行内交错。 */
    fprintf(stderr, "%s%s", message, length && message[length - 1] == '\n' ? "" : "\n");
  }
  errno = saved;
}

void lx_log_printf(lx_log_level level, const char *format, ...) {
  int saved = errno;
  char message[4096];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  lx_log_write(level, message);
  errno = saved;
}
