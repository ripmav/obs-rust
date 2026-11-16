// S-001 §4.2 (R20-R25) — clean-room implementation (see base-logging.h).

#include "base-logging.h"

#include <stdio.h>
#include <stdlib.h>

#define LOG_BUF_SIZE 8192

static void def_log_handler(int lvl, const char *fmt, va_list args, void *p)
{
	const char *prefix;
	FILE *out;

	(void)p;

	switch (lvl) {
	case LOG_DEBUG:
		prefix = "debug: ";
		out = stdout;
		break;
	case LOG_INFO:
		prefix = "info: ";
		out = stdout;
		break;
	case LOG_WARNING:
		prefix = "warning: ";
		out = stdout;
		break;
	case LOG_ERROR:
		prefix = "error: ";
		out = stderr;
		break;
	default:
		return; /* Q-3: silently dropped */
	}

	char buf[LOG_BUF_SIZE];
	vsnprintf(buf, sizeof(buf), fmt, args); /* longer messages truncated */
	fprintf(out, "%s%s\n", prefix, buf);
	fflush(out);
}

static void def_crash_handler(const char *fmt, va_list args, void *p)
{
	char buf[LOG_BUF_SIZE];

	(void)p;
	vsnprintf(buf, sizeof(buf), fmt, args);
	fprintf(stderr, "%s", buf); /* no trailing newline (R24) */
	fflush(stderr);
	exit(0);
}

static log_handler_t cur_log_handler = def_log_handler; /* initially default */
static void *cur_log_param;
static crash_handler_t cur_crash_handler;
static void *cur_crash_param;
static int crash_handler_set; /* installed at startup; single-threaded (R25) */
static int crashing;

void base_get_log_handler(log_handler_t *handler, void **param)
{
	if (handler)
		*handler = cur_log_handler;
	if (param)
		*param = cur_log_param;
}

void base_set_log_handler(log_handler_t handler, void *param)
{
	cur_log_param = param;
	if (handler == NULL)
		cur_log_handler = def_log_handler; /* NULL reinstalls the default */
	else
		cur_log_handler = handler;
}

void base_set_crash_handler(crash_handler_t handler, void *param)
{
	if (crash_handler_set) {
		blog(LOG_WARNING, "Tried to set a crash handler when one already exists.");
		return;
	}
	crash_handler_set = 1;
	cur_crash_handler = handler;
	cur_crash_param = param;
}

void blogva(int log_level, const char *format, va_list args)
{
	/* R21: the handler gets the unformatted format + argument list */
	cur_log_handler(log_level, format, args, cur_log_param);
}

void blog(int log_level, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	cur_log_handler(log_level, format, args, cur_log_param);
	va_end(args);
}

void obs_ffi_log(int level, const char *msg)
{
	blog(level, "%s", msg);
}

void bcrash(const char *format, ...)
{
	va_list args;

	if (crashing) {
		fprintf(stderr, "Crashed in the crash handler");
		fflush(stderr);
		exit(2);
	}
	crashing = 1;
	va_start(args, format);
	if (cur_crash_handler)
		cur_crash_handler(format, args, cur_crash_param);
	else
		def_crash_handler(format, args, NULL);
	va_end(args);
	exit(0); /* the handler returned */
}
