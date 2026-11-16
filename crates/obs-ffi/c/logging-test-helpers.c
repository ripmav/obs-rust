// Test support for the S-001 §4.2 scenario tests
// (tests/logging_scenarios.rs): C handler callbacks, because a `va_list`
// handler signature cannot be defined in Rust (primer §5.5). Compiled into
// the crate's static library; never called from production code.

#include "base-logging.h"

#include <stdio.h>

/* R21 capture: prints the RAW format string, then the formatted result */
void log_test_cap(int lvl, const char *fmt, va_list args, void *p)
{
	char buf[16384];
	(void)p;
	vsnprintf(buf, sizeof(buf), fmt, args);
	fprintf(stdout, "lvl=%d fmt=%s out=%s\n", lvl, fmt, buf);
	fflush(stdout);
}

void log_test_crash_a(const char *fmt, va_list args, void *p)
{
	char buf[256];
	(void)p;
	vsnprintf(buf, sizeof(buf), fmt, args);
	fprintf(stderr, "A %s", buf);
	fflush(stderr);
	bcrash("inner"); /* crash while a crash is in progress */
}

void log_test_crash_b(const char *fmt, va_list args, void *p)
{
	char buf[256];
	(void)p;
	vsnprintf(buf, sizeof(buf), fmt, args);
	fprintf(stderr, "B %s", buf);
	fflush(stderr);
	/* returning -> bcrash must exit(0) */
}
