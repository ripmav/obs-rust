/*
 * Conformance runner for bmem/base (spec S-001).
 *
 *   alloc <id> <size>        bmalloc
 *   zalloc <id> <size>       bzalloc
 *   realloc <id> <size>      brealloc (id may be unset -> acts like malloc)
 *   free <id>                bfree (unset id -> bfree(NULL))
 *   fill <id> <size> <byte>  memset a block
 *   check <id> <size> <byte> verify a block is filled with <byte>
 *   dup <id> <src> <size>    bmemdup
 *   strdup <id> <string|@null>      bstrdup
 *   strdup_n <id> <string> <n>      bstrdup_n
 *   aligned <id>             print whether the pointer is 32-byte aligned
 *   count                    print bnum_allocs() relative to the start of the script
 *   alignment                print base_get_alignment()
 *   fatal alloc0|realloc0|memdup0|realloc_null0   run the fatal path in a child process
 *   log <level> <text>       emit through blog
 *   crash_handler            install a crash handler that prints the message
 */
#include <stdint.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util/bmem.h"
#include "harness.h"

#define MAXID 16
static void *blocks[MAXID];
static long base_count;

static int id_of(const char *s)
{
	int id = atoi(s);
	if (id < 0 || id >= MAXID) {
		fprintf(stderr, "script: bad id %s\n", s);
		exit(2);
	}
	return id;
}

static void crash_to_stdout(const char *fmt, va_list args, void *param)
{
	char msg[512];
	(void)param;
	vsnprintf(msg, sizeof(msg), fmt, args);
	printf("crash %s\n", msg);
	fflush(stdout);
	_exit(77);
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;
	base_count = bnum_allocs();
	base_set_crash_handler(crash_to_stdout, NULL);

	while (script_next(f, &sl, &lineno)) {
		const char *c = sl.argv[0];
		if (!strcmp(c, "alloc")) {
			script_need(&sl, 3);
			blocks[id_of(sl.argv[1])] = bmalloc((size_t)strtoull(sl.argv[2], NULL, 10));
		} else if (!strcmp(c, "zalloc")) {
			script_need(&sl, 3);
			blocks[id_of(sl.argv[1])] = bzalloc((size_t)strtoull(sl.argv[2], NULL, 10));
		} else if (!strcmp(c, "realloc")) {
			script_need(&sl, 3);
			int id = id_of(sl.argv[1]);
			blocks[id] = brealloc(blocks[id], (size_t)strtoull(sl.argv[2], NULL, 10));
		} else if (!strcmp(c, "free")) {
			script_need(&sl, 2);
			int id = id_of(sl.argv[1]);
			bfree(blocks[id]);
			blocks[id] = NULL;
		} else if (!strcmp(c, "fill")) {
			script_need(&sl, 4);
			memset(blocks[id_of(sl.argv[1])], (int)strtol(sl.argv[3], NULL, 0),
			       (size_t)strtoull(sl.argv[2], NULL, 10));
		} else if (!strcmp(c, "check")) {
			script_need(&sl, 4);
			const uint8_t *p = blocks[id_of(sl.argv[1])];
			size_t n = (size_t)strtoull(sl.argv[2], NULL, 10);
			int want = (int)strtol(sl.argv[3], NULL, 0);
			bool ok = true;
			for (size_t i = 0; i < n; i++)
				if (p[i] != want)
					ok = false;
			printf("check %s %s\n", sl.argv[1], ok ? "ok" : "mismatch");
		} else if (!strcmp(c, "dup")) {
			script_need(&sl, 4);
			blocks[id_of(sl.argv[1])] = bmemdup(blocks[id_of(sl.argv[2])],
							    (size_t)strtoull(sl.argv[3], NULL, 10));
		} else if (!strcmp(c, "strdup")) {
			script_need(&sl, 3);
			const char *s = strcmp(sl.argv[2], "@null") ? sl.argv[2] : NULL;
			char *r = bstrdup(s);
			blocks[id_of(sl.argv[1])] = r;
			printf("strdup %s %s\n", sl.argv[1], r ? r : "@null");
		} else if (!strcmp(c, "strdup_n")) {
			script_need(&sl, 4);
			char *r = bstrdup_n(sl.argv[2], (size_t)strtoull(sl.argv[3], NULL, 10));
			blocks[id_of(sl.argv[1])] = r;
			printf("strdup_n %s %s\n", sl.argv[1], r);
		} else if (!strcmp(c, "aligned")) {
			script_need(&sl, 2);
			uintptr_t p = (uintptr_t)blocks[id_of(sl.argv[1])];
			printf("aligned %s %s\n", sl.argv[1], (p % 32) == 0 ? "yes" : "no");
		} else if (!strcmp(c, "count")) {
			printf("count %ld\n", bnum_allocs() - base_count);
		} else if (!strcmp(c, "alignment")) {
			printf("alignment %d\n", base_get_alignment());
		} else if (!strcmp(c, "log")) {
			script_need(&sl, 3);
			int lvl = !strcmp(sl.argv[1], "error")     ? LOG_ERROR
				  : !strcmp(sl.argv[1], "warning") ? LOG_WARNING
				  : !strcmp(sl.argv[1], "info")    ? LOG_INFO
								   : LOG_DEBUG;
			blog(lvl, "%s value=%d", sl.argv[2], 42);
		} else if (!strcmp(c, "fatal")) {
			script_need(&sl, 2);
			fflush(stdout);
			pid_t pid = fork();
			if (pid == 0) {
				if (!strcmp(sl.argv[1], "alloc0"))
					(void)bmalloc(0);
				else if (!strcmp(sl.argv[1], "realloc0"))
					(void)brealloc(bmalloc(4), 0);
				else if (!strcmp(sl.argv[1], "realloc_null0"))
					(void)brealloc(NULL, 0);
				else if (!strcmp(sl.argv[1], "memdup0"))
					(void)bmemdup("x", 0);
				else if (!strcmp(sl.argv[1], "zalloc0"))
					(void)bzalloc(0);
				printf("fatal %s survived\n", sl.argv[1]);
				fflush(stdout);
				_exit(0);
			}
			int st = 0;
			waitpid(pid, &st, 0);
			printf("fatal %s exit=%d\n", sl.argv[1], WIFEXITED(st) ? WEXITSTATUS(st) : -1);
		} else {
			fprintf(stderr, "script: line %d: unknown command '%s'\n", sl.lineno, c);
			return 2;
		}
	}
	fclose(f);
	return 0;
}
