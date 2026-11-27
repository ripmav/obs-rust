/* WP-40 ABI harness: the single trace channel (P0-15).
 *
 * Both the host (abi-host.c) and the plugin (trace-plugin.c) append lines to
 * the same file, whose path is the ABI_TRACE environment variable (the host
 * sets it to argv[1] before loading the module). Host and plugin run in one
 * process; the harness drives all callbacks synchronously and drains the
 * destroy queue after every release, but plugin destroy callbacks run on
 * the destroy thread, so every line is ONE write() under a mutex (glibc
 * FILE* streams are not thread-safe). O_APPEND makes each write land at the
 * current end of file, so the two binaries' fds can never overwrite each
 * other.
 *
 * Normalisation rule (critical): lines contain literals and values only —
 * never pointers, addresses, uuids or times. The line format must be
 * identical in the C build and the Rust build; byte-identical traces are
 * the conformance proof.
 */
#pragma once

#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define ABI_TRACE_LINE_MAX 1024

static inline void abi_trace(const char *kind, const char *event, const char *fmt, ...)
{
	static int fd = -1;
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	char line[ABI_TRACE_LINE_MAX];
	int n = snprintf(line, sizeof(line), "%s %s", kind, event);
	va_list ap;

	if (fmt && fmt[0]) {
		if (n + 1 < (int)sizeof(line))
			line[n++] = ' ';
		va_start(ap, fmt);
		n += vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
		va_end(ap);
	}
	if (n < 0 || n >= (int)sizeof(line) - 1)
		return;
	line[n++] = '\n';

	pthread_mutex_lock(&lock);
	if (fd < 0) {
		const char *path = getenv("ABI_TRACE");
		if (path)
			fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);
	}
	if (fd >= 0)
		(void)!write(fd, line, (size_t)n);
	pthread_mutex_unlock(&lock);
}
