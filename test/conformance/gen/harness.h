/*
 * Conformance harness: shared helpers for the runners (log capture, main loop).
 * Test tooling, not part of libobs.
 */
#pragma once

#include <stdarg.h>
#include <stdio.h>

#include "util/base.h"
#include "script.h"

/* Every message libobs logs becomes a line "log <level> <message>" on stdout so
 * that logging is part of the expected output. */
static void harness_log_handler(int level, const char *format, va_list args, void *param)
{
	char msg[4096];
	(void)param;
	vsnprintf(msg, sizeof(msg), format, args);
	for (char *p = msg; *p; p++)
		if (*p == '\n')
			*p = ' ';
	const char *name = level == LOG_ERROR     ? "error"
			   : level == LOG_WARNING ? "warning"
			   : level == LOG_INFO    ? "info"
					       : "debug";
	printf("log %s %s\n", name, msg);
}

static inline FILE *harness_open(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <script>\n", argv[0]);
		exit(2);
	}
	FILE *f = fopen(argv[1], "r");
	if (!f) {
		perror(argv[1]);
		exit(2);
	}
	base_set_log_handler(harness_log_handler, NULL);
	setvbuf(stdout, NULL, _IOLBF, 0);
	return f;
}
