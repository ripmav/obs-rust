// S-001 §4.2 (R20-R25) — clean-room port of the libobs `util/base.c` logging
// surface. The variadic entry points and the `va_list` handler callbacks
// cannot be written in stable Rust (primer §5.5); Rust logs through
// `obs_ffi_log`.

#ifndef OBS_FFI_BASE_LOGGING_H
#define OBS_FFI_BASE_LOGGING_H

#include <stdarg.h>

enum { LOG_ERROR = 100, LOG_WARNING = 200, LOG_INFO = 300, LOG_DEBUG = 400 };

typedef void (*log_handler_t)(int lvl, const char *msg, va_list args, void *p);
typedef void (*crash_handler_t)(const char *fmt, va_list args, void *p);

void base_get_log_handler(log_handler_t *handler, void **param);
void base_set_log_handler(log_handler_t handler, void *param);
void base_set_crash_handler(crash_handler_t handler, void *param);
void blogva(int log_level, const char *format, va_list args);
void blog(int log_level, const char *format, ...);
void bcrash(const char *format, ...); /* never returns */

/* Non-variadic entry for Rust: logs an already-formatted message by calling
 * blog(level, "%s", msg), so it flows through the installed handler (R21). */
void obs_ffi_log(int level, const char *msg);

#endif /* OBS_FFI_BASE_LOGGING_H */
