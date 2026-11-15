// Child-process scenario driver for the S-001 §4.2 (R20-R25) unit tests.
// Built by build.rs; invoked by the Rust tests with a mode argument.
// Each mode exits with a deterministic stdout/stderr/exit-code signature.

#include "base-logging.c"

#include <string.h>

/* R21 capture: prints the RAW format string, then the formatted result */
static void cap_handler(int lvl, const char *fmt, va_list args, void *p)
{
	char buf[16384];
	(void)p;
	vsnprintf(buf, sizeof(buf), fmt, args);
	fprintf(stdout, "lvl=%d fmt=%s out=%s\n", lvl, fmt, buf);
	fflush(stdout);
}

static void crash_a(const char *fmt, va_list args, void *p)
{
	char buf[256];
	(void)p;
	vsnprintf(buf, sizeof(buf), fmt, args);
	fprintf(stderr, "A %s", buf);
	fflush(stderr);
	bcrash("inner"); /* crash while a crash is in progress */
}

static void crash_b(const char *fmt, va_list args, void *p)
{
	char buf[256];
	(void)p;
	vsnprintf(buf, sizeof(buf), fmt, args);
	fprintf(stderr, "B %s", buf);
	fflush(stderr);
	/* returning -> bcrash must exit(0) */
}

int main(int argc, char **argv)
{
	if (argc < 2)
		return 3;
	const char *mode = argv[1];

	if (strcmp(mode, "default") == 0) {
		/* R23: per-level prefixes/streams, Q-3 drop, truncation at 8192 */
		blog(LOG_ERROR, "e1");
		blog(LOG_WARNING, "w1");
		blog(LOG_INFO, "i1 %d", 7);
		blog(LOG_DEBUG, "d1");
		blog(999, "dropped");
		blog(LOG_INFO, "%*s", 3, "padded");
		char big[9000];
		memset(big, 'x', sizeof(big) - 1);
		big[sizeof(big) - 1] = 0;
		blog(LOG_INFO, "%s", big);
		return 0;
	}

	if (strcmp(mode, "fmt") == 0) {
		/* R21: the handler receives the unformatted format + param (R22) */
		base_set_log_handler(cap_handler, (void *)0x1234);
		blog(400, "a=%d b=%s", 7, "x");
		return 0;
	}

	if (strcmp(mode, "crash-default") == 0) {
		bcrash("boom %d", 42);
		return 1; /* unreachable */
	}

	if (strcmp(mode, "crash-nested") == 0) {
		base_set_crash_handler(crash_a, NULL);
		bcrash("outer");
		return 1;
	}

	if (strcmp(mode, "crash-return") == 0) {
		base_set_crash_handler(crash_b, NULL);
		bcrash("ret %s", "v");
		return 1;
	}

	if (strcmp(mode, "once") == 0) {
		/* R25: the second install is ignored (warning) and h1 stays active */
		base_set_crash_handler(crash_b, NULL);
		base_set_crash_handler(crash_a, NULL);
		bcrash("who");
		return 1;
	}

	return 3;
}
