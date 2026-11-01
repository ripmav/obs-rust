/*
 * Conformance runner for the platform layer (spec S-023): utf8.c, the portable
 * parts of platform.c and the Linux part of platform-nix.c.
 *
 * Every file system object lives in <CONFORMANCE_TMPDIR>/platform ("the work
 * directory", W). The runner empties W before and after the script and makes it
 * the current directory, so scripts use relative paths only. Absolute paths that
 * a function returns are printed with W replaced by <W>.
 * The language is documented in test/conformance/README.md.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <ftw.h>
#include <glob.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#include "util/platform.h"
#include "util/utf8.h"
#include "util/bmem.h"
#include "media-io/video-io.h"
#include "obs.h"
#include "harness.h"

struct obs_video_info *conf_video_info(void);
void conf_set_now(int64_t sec);

static char work[PATH_MAX];  /* W as created */
static char workr[PATH_MAX]; /* W after realpath */

/* ---------------------------------------------------------------- output helpers */

/* Prints s (quoted, to the first NUL) with W replaced by <W>; NULL prints @null. */
static void printq(const char *s)
{
	if (!s) {
		fputs("@null", stdout);
		return;
	}
	char out[16384];
	size_t o = 0;
	for (const char *p = s; *p && o < sizeof(out) - 8;) {
		size_t l1 = strlen(workr), l2 = strlen(work);
		if (l1 && !strncmp(p, workr, l1) && (p[l1] == '/' || p[l1] == 0 || p[l1] == '"')) {
			memcpy(out + o, "<W>", 3), o += 3, p += l1;
		} else if (l2 && !strncmp(p, work, l2) && (p[l2] == '/' || p[l2] == 0 || p[l2] == '"')) {
			memcpy(out + o, "<W>", 3), o += 3, p += l2;
		} else {
			out[o++] = *p++;
		}
	}
	out[o] = 0;
	script_print_quoted(out);
}

static void log_handler(int level, const char *format, va_list args, void *param)
{
	char msg[4096];
	(void)param;
	vsnprintf(msg, sizeof(msg), format, args);
	for (char *p = msg; *p; p++)
		if (*p == '\n')
			*p = ' ';
	/* the text after "):" of an os_dlopen failure is dlerror() output: not portable */
	if (!strncmp(msg, "os_dlopen(", 10)) {
		char *e = strstr(msg, "):");
		if (e)
			e[2] = 0;
	}
	const char *name = level == LOG_ERROR     ? "error"
			   : level == LOG_WARNING ? "warning"
			   : level == LOG_INFO    ? "info"
						  : "debug";
	printf("log %s ", name);
	printq(msg);
	printf("\n");
}

/* The default crash handler of the base layer exits with status 0, which a script could not tell from
 * success: print the message and exit with status 77 instead. */
static void crash_handler(const char *fmt, va_list args, void *param)
{
	char msg[2048];
	(void)param;
	vsnprintf(msg, sizeof(msg), fmt, args);
	for (char *p = msg; *p; p++)
		if (*p == '\n')
			*p = ' ';
	printf("crash %s\n", msg);
	fflush(stdout);
	_exit(77);
}

static int rm_cb(const char *p, const struct stat *sb, int type, struct FTW *ftw)
{
	(void)sb, (void)type, (void)ftw;
	if (ftw->level > 0)
		remove(p);
	return 0;
}
static void empty_dir(const char *p)
{
	nftw(p, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
}

static const char *S(const struct script_line *sl, int i)
{
	return strcmp(sl->argv[i], "@null") ? sl->argv[i] : NULL;
}
static long long N(const struct script_line *sl, int i)
{
	return strtoll(sl->argv[i], NULL, 0);
}
/* length argument: a number, "auto" (= 0, meaning "to the terminator") or "len" (= byte length of txt) */
static size_t LEN(const struct script_line *sl, int i, size_t len)
{
	if (!strcmp(sl->argv[i], "auto"))
		return 0;
	if (!strcmp(sl->argv[i], "len"))
		return len;
	return (size_t)strtoull(sl->argv[i], NULL, 0);
}

/* "63,d800,10ffff" -> wchar_t array (count in *n); "-" is empty. Caller frees. */
static wchar_t *parse_cps(const char *s, size_t *n)
{
	size_t cap = strlen(s) / 2 + 2, cnt = 0;
	wchar_t *w = malloc(cap * sizeof(wchar_t) + sizeof(wchar_t));
	if (strcmp(s, "-")) {
		const char *p = s;
		while (*p) {
			char *e;
			w[cnt++] = (wchar_t)(int32_t)strtoul(p, &e, 16);
			p = *e == ',' ? e + 1 : e;
		}
	}
	w[cnt] = 0; /* terminator for the "to the terminator" mode */
	*n = cnt;
	return w;
}
static void print_cps(const wchar_t *w, size_t n)
{
	if (!n)
		fputs("-", stdout);
	for (size_t i = 0; i < n; i++)
		printf("%s%x", i ? "," : "", (unsigned)w[i]);
}
static void print_hexbytes(const char *b, size_t n)
{
	script_print_hex((const uint8_t *)b, n);
}

/* ---------------------------------------------------------------- FILE handles */

#define MAXH 16
static struct {
	char name[32];
	FILE *f;
} handles[MAXH];

static FILE **H(const char *name, bool create)
{
	for (int i = 0; i < MAXH; i++)
		if (handles[i].name[0] && !strcmp(handles[i].name, name))
			return &handles[i].f;
	if (create)
		for (int i = 0; i < MAXH; i++)
			if (!handles[i].name[0]) {
				snprintf(handles[i].name, sizeof handles[i].name, "%s", name);
				handles[i].f = NULL;
				return &handles[i].f;
			}
	fprintf(stderr, "script: unknown or too many handles: %s\n", name);
	exit(2);
}

static uint32_t fnv(const unsigned char *b, size_t n)
{
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < n; i++)
		h = (h ^ b[i]) * 16777619u;
	return h;
}

static int cmp_str(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static enum video_format format_by_name(const char *n)
{
	static const struct {
		const char *n;
		enum video_format f;
	} t[] = {{"NONE", VIDEO_FORMAT_NONE}, {"I420", VIDEO_FORMAT_I420}, {"NV12", VIDEO_FORMAT_NV12},
		 {"YVYU", VIDEO_FORMAT_YVYU}, {"YUY2", VIDEO_FORMAT_YUY2}, {"UYVY", VIDEO_FORMAT_UYVY},
		 {"RGBA", VIDEO_FORMAT_RGBA}, {"BGRA", VIDEO_FORMAT_BGRA}, {"BGRX", VIDEO_FORMAT_BGRX},
		 {"Y800", VIDEO_FORMAT_Y800}, {"I444", VIDEO_FORMAT_I444}, {"BGR3", VIDEO_FORMAT_BGR3},
		 {"I422", VIDEO_FORMAT_I422}, {"I40A", VIDEO_FORMAT_I40A}, {"I42A", VIDEO_FORMAT_I42A},
		 {"YUVA", VIDEO_FORMAT_YUVA}, {"AYUV", VIDEO_FORMAT_AYUV}, {"I010", VIDEO_FORMAT_I010},
		 {"P010", VIDEO_FORMAT_P010}, {"I210", VIDEO_FORMAT_I210}, {"I412", VIDEO_FORMAT_I412},
		 {"YA2L", VIDEO_FORMAT_YA2L}, {"P216", VIDEO_FORMAT_P216}, {"P416", VIDEO_FORMAT_P416},
		 {"V210", VIDEO_FORMAT_V210}, {"R10L", VIDEO_FORMAT_R10L}};
	for (size_t i = 0; i < sizeof t / sizeof *t; i++)
		if (!strcmp(t[i].n, n))
			return t[i].f;
	fprintf(stderr, "script: unknown video format %s\n", n);
	exit(2);
}

static uint64_t base_ns;

static int64_t rel(const char *s)
{
	return (int64_t)base_ns + strtoll(s, NULL, 0);
}

#define BAD(sl)                                                                 \
	do {                                                                    \
		fprintf(stderr, "script: line %d: bad command '%s'\n", (sl)->lineno, (sl)->argv[0]); \
		exit(2);                                                        \
	} while (0)

#define IS(c) (!strcmp(cmd, c))

static void run(struct script_line *sl);

/* ---------------------------------------------------------------- commands */

static void cmd_try(struct script_line *sl)
{
	/* try [quiet] CMD ARGS... : run the command in a child process and report how it ended.
	 * try_defect [quiet]     : same, but the line is printed "try_defect CMD ..." (a reference crash there
	 *                          is accepted as "ok" by a conforming implementation, see README).
	 * quiet silences what the child prints. */
	bool defect = !strcmp(sl->argv[0], "try_defect");
	bool quiet = !strcmp(sl->argv[1], "quiet");
	int shift = quiet ? 2 : 1;
	script_need(sl, shift + 1);
	struct script_line sub;
	sub.lineno = sl->lineno;
	sub.argc = sl->argc - shift;
	for (int i = 0; i < sub.argc; i++)
		sub.argv[i] = sl->argv[i + shift];
	fflush(stdout);
	pid_t pid = fork();
	if (pid == 0) {
		if (quiet)
			freopen("/dev/null", "w", stdout);
		freopen("/dev/null", "w", stderr);
		run(&sub);
		fflush(stdout);
		_exit(0);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	const char *tag = defect ? "try_defect" : "try";
	if (WIFSIGNALED(st))
		printf("%s %s crash\n", tag, sub.argv[0]);
	else if (WEXITSTATUS(st) == 0)
		printf("%s %s ok\n", tag, sub.argv[0]);
	else
		printf("%s %s exit-%d\n", tag, sub.argv[0], WEXITSTATUS(st));
}

static void print_buf_result(const char *tag, size_t rc, const char *dst)
{
	printf("%s rc=%zu str=", tag, rc);
	printq(dst);
	printf("\n");
}

static void run(struct script_line *sl)
{
	const char *cmd = sl->argv[0];

	/* ------------------------------------------------ utf8.c */
	if (IS("u2w")) { /* FLAGS INSIZE HEX OUT */
		script_need(sl, 5);
		if (!strcmp(sl->argv[3], "@null")) {
			size_t rc = utf8_to_wchar(NULL, (size_t)N(sl, 2), NULL, 0, (int)N(sl, 1));
			printf("u2w rc=%zu\n", rc);
			return;
		}
		size_t n;
		uint8_t *in = script_parse_hex(sl->argv[3], &n);
		char *buf = malloc(n + 1);
		memcpy(buf, in, n);
		buf[n] = 0;
		free(in);
		size_t insize = LEN(sl, 2, n);
		bool sizing = !strcmp(sl->argv[4], "null");
		size_t cap = sizing ? 0 : (size_t)N(sl, 4);
		wchar_t *out = sizing ? NULL : malloc(cap ? cap * sizeof(wchar_t) : 1);
		size_t rc = utf8_to_wchar(buf, insize, out, cap, (int)N(sl, 1));
		printf("u2w rc=%zu", rc);
		if (out && rc && rc <= cap) {
			printf(" cp=");
			print_cps(out, rc);
		}
		printf("\n");
		free(out);
		free(buf);
	} else if (IS("w2u")) { /* FLAGS INSIZE CPS OUT */
		script_need(sl, 5);
		if (!strcmp(sl->argv[3], "@null")) {
			size_t rc = wchar_to_utf8(NULL, (size_t)N(sl, 2), NULL, 0, (int)N(sl, 1));
			printf("w2u rc=%zu\n", rc);
			return;
		}
		size_t n;
		wchar_t *in = parse_cps(sl->argv[3], &n);
		size_t insize = LEN(sl, 2, n);
		bool sizing = !strcmp(sl->argv[4], "null");
		size_t cap = sizing ? 0 : (size_t)N(sl, 4);
		char *out = sizing ? NULL : malloc(cap ? cap : 1);
		size_t rc = wchar_to_utf8(in, insize, out, cap, (int)N(sl, 1));
		printf("w2u rc=%zu", rc);
		if (out && rc && rc <= cap) {
			printf(" hex=");
			print_hexbytes(out, rc);
		}
		printf("\n");
		free(out);
		free(in);
	}
	/* ------------------------------------------------ platform.c string conversions */
	else if (IS("os_u2w")) { /* DSTSIZE|null LEN TEXT|@null */
		script_need(sl, 4);
		bool sizing = !strcmp(sl->argv[1], "null");
		size_t cap = sizing ? 0 : (size_t)N(sl, 1);
		size_t n;
		uint8_t *in = script_parse_hex(sl->argv[3], &n);
		char *buf = malloc(n + 1);
		memcpy(buf, in, n);
		buf[n] = 0;
		free(in);
		const char *str = strcmp(sl->argv[3], "@null") ? buf : NULL;
		wchar_t *dst = sizing ? NULL : malloc((cap ? cap : 1) * sizeof(wchar_t));
		size_t rc = os_utf8_to_wcs(str, LEN(sl, 2, n), dst, cap);
		printf("os_u2w rc=%zu", rc);
		if (dst && cap && str) { /* with a NULL source the buffer is not touched */
			printf(" cp=");
			print_cps(dst, rc);
			printf(" term=%d", dst[rc] == 0);
		}
		printf("\n");
		free(dst);
		free(buf);
	} else if (IS("os_w2u")) { /* DSTSIZE|null LEN CPS|@null */
		script_need(sl, 4);
		bool sizing = !strcmp(sl->argv[1], "null");
		size_t cap = sizing ? 0 : (size_t)N(sl, 1);
		size_t n = 0;
		bool isnull = !strcmp(sl->argv[3], "@null");
		wchar_t *in = isnull ? NULL : parse_cps(sl->argv[3], &n);
		char *dst = sizing ? NULL : malloc(cap ? cap : 1);
		size_t rc = os_wcs_to_utf8(in, LEN(sl, 2, n), dst, cap);
		printf("os_w2u rc=%zu", rc);
		if (dst && cap && in) {
			printf(" hex=");
			print_hexbytes(dst, rc);
			printf(" term=%d", dst[rc] == 0);
		}
		printf("\n");
		free(dst);
		free(in);
	} else if (IS("os_u2w_ptr")) { /* LEN HEX|@null */
		script_need(sl, 3);
		size_t n;
		uint8_t *in = script_parse_hex(sl->argv[2], &n);
		char *buf = malloc(n + 1);
		memcpy(buf, in, n);
		buf[n] = 0;
		free(in);
		wchar_t *dst = (wchar_t *)0x1;
		size_t rc = os_utf8_to_wcs_ptr(strcmp(sl->argv[2], "@null") ? buf : NULL, LEN(sl, 1, n), &dst);
		printf("os_u2w_ptr rc=%zu ", rc);
		if (dst) {
			printf("cp=");
			print_cps(dst, rc);
			printf(" term=%d", dst[rc] == 0);
		} else {
			printf("ptr=null");
		}
		printf("\n");
		bfree(dst);
		free(buf);
	} else if (IS("os_w2u_ptr")) { /* LEN CPS|@null */
		script_need(sl, 3);
		size_t n = 0;
		bool isnull = !strcmp(sl->argv[2], "@null");
		wchar_t *in = isnull ? NULL : parse_cps(sl->argv[2], &n);
		char *dst = (char *)0x1;
		size_t rc = os_wcs_to_utf8_ptr(in, LEN(sl, 1, n), &dst);
		printf("os_w2u_ptr rc=%zu ", rc);
		if (dst) {
			printf("hex=");
			print_hexbytes(dst, rc);
			printf(" term=%d", dst[rc] == 0);
		} else {
			printf("ptr=null");
		}
		printf("\n");
		bfree(dst);
		free(in);
	} else if (IS("os_mbs2wcs")) { /* DSTSIZE|null TEXT|@null  (ASCII text only) */
		script_need(sl, 3);
		bool sizing = !strcmp(sl->argv[1], "null");
		size_t cap = sizing ? 0 : (size_t)N(sl, 1);
		wchar_t *dst = sizing ? NULL : malloc((cap ? cap : 1) * sizeof(wchar_t));
		size_t rc = os_mbs_to_wcs(S(sl, 2), 0, dst, cap);
		printf("os_mbs2wcs rc=%zu", rc);
		if (dst && cap && S(sl, 2)) {
			printf(" cp=");
			print_cps(dst, rc);
		}
		printf("\n");
		free(dst);
	} else if (IS("os_wcs2mbs")) { /* DSTSIZE|null CPS|@null (ASCII only) */
		script_need(sl, 3);
		bool sizing = !strcmp(sl->argv[1], "null");
		size_t cap = sizing ? 0 : (size_t)N(sl, 1);
		size_t n = 0;
		wchar_t *in = !strcmp(sl->argv[2], "@null") ? NULL : parse_cps(sl->argv[2], &n);
		char *dst = sizing ? NULL : malloc(cap ? cap : 1);
		size_t rc = os_wcs_to_mbs(in, 0, dst, cap);
		printf("os_wcs2mbs rc=%zu", rc);
		if (dst && cap && in) {
			printf(" str=");
			printq(dst);
		}
		printf("\n");
		free(dst);
		free(in);
	} else if (IS("os_mbs2wcs_ptr")) { /* TEXT|@null */
		script_need(sl, 2);
		wchar_t *dst = (wchar_t *)0x1;
		size_t rc = os_mbs_to_wcs_ptr(S(sl, 1), 0, &dst);
		printf("os_mbs2wcs_ptr rc=%zu ", rc);
		if (dst) {
			printf("cp=");
			print_cps(dst, rc);
		} else
			printf("ptr=null");
		printf("\n");
		bfree(dst);
	} else if (IS("os_wcs2mbs_ptr")) { /* CPS|@null */
		script_need(sl, 2);
		size_t n = 0;
		wchar_t *in = !strcmp(sl->argv[1], "@null") ? NULL : parse_cps(sl->argv[1], &n);
		char *dst = (char *)0x1;
		size_t rc = os_wcs_to_mbs_ptr(in, 0, &dst);
		printf("os_wcs2mbs_ptr rc=%zu str=", rc);
		printq(dst);
		printf("\n");
		bfree(dst);
		free(in);
	} else if (IS("os_u2mbs_ptr") || IS("os_mbs2u_ptr")) { /* LEN TEXT|@null (ASCII only) */
		script_need(sl, 3);
		char *dst = (char *)0x1;
		const char *t = S(sl, 2);
		size_t l = LEN(sl, 1, t ? strlen(t) : 0);
		size_t rc = IS("os_u2mbs_ptr") ? os_utf8_to_mbs_ptr(t, l, &dst) : os_mbs_to_utf8_ptr(t, l, &dst);
		printf("%s rc=%zu str=", cmd, rc);
		printq(dst);
		printf("\n");
		bfree(dst);
	}
	/* ------------------------------------------------ number helpers */
	else if (IS("dtostr")) { /* VALUE SIZE */
		script_need(sl, 3);
		size_t size = (size_t)N(sl, 2);
		char *buf = malloc(size ? size : 1);
		memset(buf, 'X', size);
		int rc = os_dtostr(strtod(sl->argv[1], NULL), buf, size);
		printf("dtostr %s size=%zu rc=%d", sl->argv[1], size, rc);
		if (rc >= 0) {
			/* The reference moves the exponent text one place too short, so the buffer can hold
			 * stale characters behind the first rc bytes (defect D-2). The value that counts is
			 * the first rc bytes; the raw C string is printed on an informational line. */
			char *cut = malloc((size_t)rc + 1);
			memcpy(cut, buf, (size_t)rc);
			cut[rc] = 0;
			printf(" str=");
			printq(cut);
			free(cut);
			if (strlen(buf) != (size_t)rc) {
				printf("\ninfo_dtostr_raw ");
				printq(buf);
			}
		}
		printf("\n");
		free(buf);
	} else if (IS("strtod")) { /* TEXT */
		script_need(sl, 2);
		printf("strtod %.17g\n", os_strtod(sl->argv[1]));
	}
	/* ------------------------------------------------ setup helpers (plain libc) */
	else if (IS("put")) { /* PATH TEXT */
		script_need(sl, 3);
		FILE *f = fopen(sl->argv[1], "wb");
		if (!f) {
			printf("put failed\n");
			return;
		}
		fwrite(sl->argv[2], 1, strlen(sl->argv[2]), f);
		fclose(f);
	} else if (IS("put_hex")) { /* PATH HEX */
		script_need(sl, 3);
		size_t n;
		uint8_t *b = script_parse_hex(sl->argv[2], &n);
		FILE *f = fopen(sl->argv[1], "wb");
		if (f) {
			fwrite(b, 1, n, f);
			fclose(f);
		}
		free(b);
	} else if (IS("put_pattern")) { /* PATH N : byte i = (i*7+3) & 0xff */
		script_need(sl, 3);
		FILE *f = fopen(sl->argv[1], "wb");
		size_t n = (size_t)N(sl, 2);
		for (size_t i = 0; f && i < n; i++)
			fputc((int)((i * 7 + 3) & 0xff), f);
		if (f)
			fclose(f);
	} else if (IS("symlink")) { /* TARGET LINK */
		script_need(sl, 3);
		printf("symlink %d\n", symlink(sl->argv[1], sl->argv[2]) == 0);
	} else if (IS("cat")) { /* PATH : raw content, to the end of the file (hex) */
		script_need(sl, 2);
		FILE *f = fopen(sl->argv[1], "rb");
		if (!f) {
			printf("cat %s nofile\n", sl->argv[1]);
			return;
		}
		static unsigned char buf[1 << 16];
		size_t n = fread(buf, 1, sizeof buf, f);
		fclose(f);
		printf("cat %s size=%zu hex=", sl->argv[1], n);
		script_print_hex(buf, n);
		printf("\n");
	} else if (IS("info")) { /* PATH : size + checksum of a regular file, or kind */
		script_need(sl, 2);
		struct stat st;
		if (lstat(sl->argv[1], &st) != 0) {
			printf("info %s none\n", sl->argv[1]);
			return;
		}
		if (S_ISDIR(st.st_mode)) {
			printf("info %s dir\n", sl->argv[1]);
		} else if (S_ISLNK(st.st_mode)) {
			printf("info %s link\n", sl->argv[1]);
		} else {
			FILE *f = fopen(sl->argv[1], "rb");
			unsigned char *b = malloc((size_t)st.st_size + 1);
			size_t n = f ? fread(b, 1, (size_t)st.st_size, f) : 0;
			if (f)
				fclose(f);
			printf("info %s file size=%zu fnv=%08x\n", sl->argv[1], n, fnv(b, n));
			free(b);
		}
	} else if (IS("echo")) {
		printf("echo");
		for (int i = 1; i < sl->argc; i++)
			printf(" %s", sl->argv[i]);
		printf("\n");
	}
	/* ------------------------------------------------ FILE based */
	else if (IS("fopen") || IS("wfopen")) { /* H PATH|@null MODE */
		script_need(sl, 4);
		FILE **h = H(sl->argv[1], true);
		if (*h) {
			fclose(*h);
			*h = NULL;
		}
		if (IS("fopen")) {
			*h = os_fopen(S(sl, 2), sl->argv[3]);
		} else {
			wchar_t *w = NULL;
			if (S(sl, 2)) {
				size_t n = utf8_to_wchar(sl->argv[2], 0, NULL, 0, 0);
				w = malloc((n + 1) * sizeof(wchar_t));
				utf8_to_wchar(sl->argv[2], 0, w, n + 1, 0);
				w[n] = 0;
			}
			*h = os_wfopen(w, sl->argv[3]);
			free(w);
		}
		printf("%s %s %s\n", cmd, sl->argv[1], *h ? "ok" : "null");
	} else if (IS("wfopen_cp")) { /* H CPS MODE */
		script_need(sl, 4);
		FILE **h = H(sl->argv[1], true);
		size_t n;
		wchar_t *w = parse_cps(sl->argv[2], &n);
		*h = os_wfopen(w, sl->argv[3]);
		free(w);
		printf("wfopen_cp %s %s\n", sl->argv[1], *h ? "ok" : "null");
	} else if (IS("fclose")) {
		script_need(sl, 2);
		FILE **h = H(sl->argv[1], false);
		printf("fclose %s %d\n", sl->argv[1], fclose(*h));
		*h = NULL;
	} else if (IS("fflush")) {
		script_need(sl, 2);
		fflush(*H(sl->argv[1], false));
	} else if (IS("fwrite")) { /* H TEXT */
		script_need(sl, 3);
		FILE *f = *H(sl->argv[1], false);
		printf("fwrite %s %zu\n", sl->argv[1], fwrite(sl->argv[2], 1, strlen(sl->argv[2]), f));
	} else if (IS("fread")) { /* H N */
		script_need(sl, 3);
		FILE *f = *H(sl->argv[1], false);
		size_t n = (size_t)N(sl, 2);
		char *b = malloc(n + 1);
		size_t got = fread(b, 1, n, f);
		b[got] = 0;
		printf("fread %s %zu hex=", sl->argv[1], got);
		script_print_hex((uint8_t *)b, got);
		printf("\n");
		free(b);
	} else if (IS("fseeki")) { /* H OFFSET set|cur|end */
		script_need(sl, 4);
		int org = !strcmp(sl->argv[3], "set") ? SEEK_SET : !strcmp(sl->argv[3], "cur") ? SEEK_CUR : SEEK_END;
		printf("fseeki %s rc=%d\n", sl->argv[1], os_fseeki64(*H(sl->argv[1], false), N(sl, 2), org));
	} else if (IS("ftelli")) {
		script_need(sl, 2);
		printf("ftelli %s %lld\n", sl->argv[1], (long long)os_ftelli64(*H(sl->argv[1], false)));
	} else if (IS("fgetsize")) {
		script_need(sl, 2);
		printf("fgetsize %s %lld\n", sl->argv[1], (long long)os_fgetsize(*H(sl->argv[1], false)));
	} else if (IS("fread_utf8") || IS("fread_mbs")) {
		script_need(sl, 2);
		char *str = (char *)0x1;
		FILE *f = *H(sl->argv[1], false);
		size_t rc = IS("fread_utf8") ? os_fread_utf8(f, &str) : os_fread_mbs(f, &str);
		printf("%s %s rc=%zu str=", cmd, sl->argv[1], rc);
		printq(str);
		printf("\n");
		bfree(str);
	}
	/* ------------------------------------------------ whole-file helpers */
	else if (IS("quick_read_utf8") || IS("quick_read_mbs")) {
		script_need(sl, 2);
		char *s = IS("quick_read_utf8") ? os_quick_read_utf8_file(S(sl, 1)) : os_quick_read_mbs_file(S(sl, 1));
		printf("%s ", cmd);
		printq(s);
		printf("\n");
		bfree(s);
	} else if (IS("quick_write_utf8")) { /* PATH TEXT|@null LEN MARKER */
		script_need(sl, 5);
		const char *t = S(sl, 2);
		printf("quick_write_utf8 %d\n",
		       os_quick_write_utf8_file(S(sl, 1), t, LEN(sl, 3, t ? strlen(t) : 0), N(sl, 4) != 0));
	} else if (IS("quick_write_utf8_safe")) { /* PATH TEXT LEN MARKER TMPEXT BAKEXT */
		script_need(sl, 7);
		printf("quick_write_utf8_safe %d\n",
		       os_quick_write_utf8_file_safe(S(sl, 1), sl->argv[2], LEN(sl, 3, strlen(sl->argv[2])), N(sl, 4) != 0,
						     S(sl, 5), S(sl, 6)));
	} else if (IS("quick_write_mbs")) { /* PATH TEXT LEN  (ASCII text) */
		script_need(sl, 4);
		printf("quick_write_mbs %d\n",
		       os_quick_write_mbs_file(S(sl, 1), sl->argv[2], LEN(sl, 3, strlen(sl->argv[2]))));
	} else if (IS("file_size")) {
		script_need(sl, 2);
		printf("file_size %s %lld\n", sl->argv[1], (long long)os_get_file_size(S(sl, 1)));
	} else if (IS("exists")) {
		script_need(sl, 2);
		printf("exists %s %d\n", sl->argv[1], os_file_exists(S(sl, 1)));
	} else if (IS("mkdir") || IS("mkdirs")) {
		script_need(sl, 2);
		printf("%s %s %d\n", cmd, sl->argv[1], IS("mkdir") ? os_mkdir(sl->argv[1]) : os_mkdirs(sl->argv[1]));
	} else if (IS("rename") || IS("copyfile")) {
		script_need(sl, 3);
		printf("%s %s %s %d\n", cmd, sl->argv[1], sl->argv[2],
		       IS("rename") ? os_rename(S(sl, 1), S(sl, 2)) : os_copyfile(S(sl, 1), S(sl, 2)));
	} else if (IS("unlink") || IS("rmdir")) {
		script_need(sl, 2);
		printf("%s %s %d\n", cmd, sl->argv[1], IS("unlink") ? os_unlink(S(sl, 1)) : os_rmdir(S(sl, 1)));
	} else if (IS("safe_replace")) { /* TARGET FROM BACKUP|@null */
		script_need(sl, 4);
		printf("safe_replace %d\n", os_safe_replace(sl->argv[1], sl->argv[2], S(sl, 3)));
	} else if (IS("ext")) { /* PATH */
		script_need(sl, 2);
		const char *e = os_get_path_extension(sl->argv[1]);
		printf("ext %s ", sl->argv[1]);
		if (e)
			printf("offset=%zu ext=", (size_t)(e - sl->argv[1]));
		printq(e);
		printf("\n");
	} else if (IS("abs_path")) { /* PATH SIZE|null */
		script_need(sl, 3);
		bool nobuf = !strcmp(sl->argv[2], "null");
		size_t size = nobuf ? 0 : (size_t)N(sl, 2);
		char *buf = nobuf ? NULL : malloc(size ? size : 1);
		if (buf)
			memset(buf, 'X', size);
		size_t rc = os_get_abs_path(S(sl, 1), buf, size);
		if (buf && size == 0) {
			if (rc)
				printf("abs_path %s size0 rel_rc=%zd\n", sl->argv[1], (ssize_t)rc - (ssize_t)strlen(workr));
			else
				printf("abs_path %s size0 rc=0\n", sl->argv[1]);
		} else if (rc >= size && size && rc > strlen(workr)) {
			/* truncated: the absolute length depends on W; show only the relative length */
			printf("abs_path %s truncated rel_rc=%zd buflen=%zu\n", sl->argv[1], (ssize_t)rc - (ssize_t)strlen(workr),
			       strlen(buf));
		} else {
			printf("abs_path %s rc=%s", sl->argv[1], rc ? "nonzero" : "0");
			if (rc) {
				printf(" str=");
				printq(buf);
				printf(" rc_is_len=%d", rc == strlen(buf));
			}
			printf("\n");
		}
		free(buf);
	} else if (IS("abs_path_ptr")) {
		script_need(sl, 2);
		char *p = os_get_abs_path_ptr(S(sl, 1));
		printf("abs_path_ptr %s ", sl->argv[1]);
		printq(p);
		printf("\n");
		bfree(p);
	} else if (IS("getcwd")) { /* SIZE */
		script_need(sl, 2);
		size_t size = (size_t)N(sl, 1);
		char *buf = malloc(size ? size : 1);
		char *r = os_getcwd(buf, size);
		printf("getcwd size=%zu ", size);
		if (r && strlen(workr) + 1 > size)
			printf("(short)");
		printq(r);
		printf("\n");
		free(buf);
	} else if (IS("chdir")) {
		script_need(sl, 2);
		printf("chdir %s %d\n", sl->argv[1], os_chdir(sl->argv[1]));
	}
	/* ------------------------------------------------ directories */
	else if (IS("ls")) { /* DIR : entries sorted bytewise; also exercises the end-of-dir protocol */
		script_need(sl, 2);
		os_dir_t *d = os_opendir(sl->argv[1]);
		if (!d) {
			printf("ls %s null\n", sl->argv[1]);
			return;
		}
		char *names[1024];
		int cnt = 0;
		struct os_dirent *e;
		while ((e = os_readdir(d)) && cnt < 1024) {
			char line[300];
			snprintf(line, sizeof line, "%s %c", e->d_name, e->directory ? 'd' : 'f');
			names[cnt++] = strdup(line);
		}
		bool again = os_readdir(d) == NULL;
		os_closedir(d);
		qsort(names, (size_t)cnt, sizeof(char *), cmp_str);
		printf("ls %s %d end_repeat=%d\n", sl->argv[1], cnt, again);
		for (int i = 0; i < cnt; i++) {
			char *sp = strrchr(names[i], ' ');
			*sp = 0;
			printf("  ");
			printq(names[i]);
			printf(" %s\n", sp + 1);
			free(names[i]);
		}
	} else if (IS("dir_nulls")) {
		printf("dir_nulls readdir=%d closedir_ok=1\n", os_readdir(NULL) == NULL);
		os_closedir(NULL);
	} else if (IS("glob")) { /* PATTERN */
		script_need(sl, 2);
		os_glob_t *g = (os_glob_t *)0x1;
		int rc = os_glob(sl->argv[1], 0xff, &g);
		printf("glob %s rc=%d", sl->argv[1], rc);
		if (rc == 0) {
			printf(" count=%zu\n", g->gl_pathc);
			for (size_t i = 0; i < g->gl_pathc; i++) {
				printf("  ");
				printq(g->gl_pathv[i].path);
				printf(" %s\n", g->gl_pathv[i].directory ? "d" : "f");
			}
		} else {
			printf(" ptr=%s\n", g ? "set" : "null");
		}
		os_globfree(g);
	} else if (IS("globfree_null")) {
		os_globfree(NULL);
		printf("globfree_null ok\n");
	}
	/* ------------------------------------------------ names and environment */
	else if (IS("setenv")) { /* NAME VALUE|@unset */
		script_need(sl, 3);
		if (!strcmp(sl->argv[2], "@unset"))
			unsetenv(sl->argv[1]);
		else
			setenv(sl->argv[1], sl->argv[2], 1);
	} else if (IS("config_path") || IS("program_data")) { /* SIZE|null NAME|@null */
		script_need(sl, 3);
		bool nobuf = !strcmp(sl->argv[1], "null");
		size_t size = nobuf ? 0 : (size_t)N(sl, 1);
		char *buf = nobuf ? NULL : malloc(size ? size : 1);
		if (buf)
			memset(buf, 'X', size);
		int rc = IS("config_path") ? os_get_config_path(buf, size, S(sl, 2))
					   : os_get_program_data_path(buf, size, S(sl, 2));
		printf("%s rc=%d", cmd, rc);
		if (buf && size) {
			printf(" str=");
			printq(buf);
		}
		printf("\n");
		free(buf);
	} else if (IS("config_path_ptr") || IS("program_data_ptr")) { /* NAME|@null */
		script_need(sl, 2);
		char *p = IS("config_path_ptr") ? os_get_config_path_ptr(S(sl, 1)) : os_get_program_data_path_ptr(S(sl, 1));
		printf("%s ", cmd);
		printq(p);
		printf("\n");
		bfree(p);
	} else if (IS("exe_path")) { /* NAME|@null : the directory depends on where the runner lives */
		script_need(sl, 2);
		char *p = os_get_executable_path_ptr(S(sl, 1));
		const char *name = S(sl, 1) ? sl->argv[1] : "";
		size_t pl = p ? strlen(p) : 0, nl = strlen(name);
		printf("exe_path ok=%d absolute=%d suffix=%d\n", p != NULL, p && p[0] == '/',
		       p && pl > nl && !strcmp(p + pl - nl, name) && p[pl - nl - 1] == '/');
		bfree(p);
	}
	/* ------------------------------------------------ formatted file names */
	else if (IS("now")) { /* EPOCH_SECONDS */
		script_need(sl, 2);
		conf_set_now(N(sl, 1));
	} else if (IS("tz")) { /* POSIX TZ string */
		script_need(sl, 2);
		setenv("TZ", sl->argv[1], 1);
		tzset();
	} else if (IS("video")) { /* FPSNUM FPSDEN BASEW BASEH OUTW OUTH FORMAT */
		script_need(sl, 8);
		struct obs_video_info *v = conf_video_info();
		v->fps_num = (uint32_t)N(sl, 1);
		v->fps_den = (uint32_t)N(sl, 2);
		v->base_width = (uint32_t)N(sl, 3);
		v->base_height = (uint32_t)N(sl, 4);
		v->output_width = (uint32_t)N(sl, 5);
		v->output_height = (uint32_t)N(sl, 6);
		v->output_format = format_by_name(sl->argv[7]);
	} else if (IS("fname")) { /* EXT|@null SPACE FORMAT|@null */
		script_need(sl, 4);
		char *r = os_generate_formatted_filename(S(sl, 1), N(sl, 2) != 0, S(sl, 3));
		printf("fname ");
		printq(r);
		if (r)
			printf(" len=%zu", strlen(r));
		printf("\n");
		bfree(r);
	}
	/* ------------------------------------------------ time and system contracts */
	else if (IS("monotonic")) { /* N */
		script_need(sl, 2);
		uint64_t prev = os_gettime_ns();
		bool ok = prev != 0;
		for (long long i = 0; i < N(sl, 1); i++) {
			uint64_t t = os_gettime_ns();
			ok = ok && t >= prev;
			prev = t;
		}
		printf("monotonic %d\n", ok);
	} else if (IS("sleep_ms")) { /* MS */
		script_need(sl, 2);
		uint64_t a = os_gettime_ns();
		os_sleep_ms((uint32_t)N(sl, 1));
		printf("sleep_ms elapsed_ge=%d\n", os_gettime_ns() - a >= (uint64_t)N(sl, 1) * 1000000ull);
	} else if (IS("sleepto") || IS("sleepto_fast")) { /* REL_MS (may be negative) */
		script_need(sl, 2);
		uint64_t target = os_gettime_ns() + (uint64_t)(N(sl, 1) * 1000000ll);
		bool rc = IS("sleepto") ? os_sleepto_ns(target) : os_sleepto_ns_fast(target);
		printf("%s rc=%d reached=%d\n", cmd, rc, N(sl, 1) < 0 ? 1 : os_gettime_ns() >= target);
	} else if (IS("nstime")) { /* A B : timestamps relative to the runner start, in ns */
		script_need(sl, 3);
		struct timespec ta, tb;
		struct timespec *ra = os_nstime_to_timespec((uint64_t)rel(sl->argv[1]), &ta);
		struct timespec *rb = os_nstime_to_timespec((uint64_t)rel(sl->argv[2]), &tb);
		if (!ra || !rb) {
			printf("nstime null\n");
			return;
		}
		long long d = ((long long)tb.tv_sec - ta.tv_sec) * 1000000000ll + (tb.tv_nsec - ta.tv_nsec);
		printf("nstime ret_is_storage=%d diff_ns=%lld nsec_in_range=%d\n", ra == &ta, d,
		       ta.tv_nsec >= 0 && ta.tv_nsec < 1000000000 && tb.tv_nsec >= 0 && tb.tv_nsec < 1000000000);
	} else if (IS("nstime_null")) {
		printf("nstime_null %d\n", os_nstime_to_timespec(1000, NULL) == NULL);
	} else if (IS("free_space")) { /* PATH */
		script_need(sl, 2);
		int64_t a = os_get_free_space(sl->argv[1]);
		uint64_t b = os_get_free_disk_space(sl->argv[1]);
		printf("free_space %s space=%s disk=%s\n", sl->argv[1], a < 0 ? "error" : "ok", b ? "nonzero" : "zero");
		if (a < 0)
			printf("free_space %s space_rc=%lld\n", sl->argv[1], (long long)a);
	} else if (IS("system")) {
		int p = os_get_physical_cores(), l = os_get_logical_cores();
		os_proc_memory_usage_t u = {0};
		bool mu = os_get_proc_memory_usage(&u);
		printf("system cores_ok=%d free_mem_pos=%d total_mem_pos=%d free_le_total=%d proc_mem=%d resident_pos=%d virtual_pos=%d\n",
		       p >= 1 && l >= 1 && p <= l, os_get_sys_free_size() > 0, os_get_sys_total_size() > 0,
		       os_get_sys_free_size() <= os_get_sys_total_size(), mu, u.resident_size > 0, u.virtual_size > 0);
		printf("system resident_fn_pos=%d virtual_fn_pos=%d emulation=%d is_obs_plugin=%d perf_token_null=%d\n",
		       os_get_proc_resident_size() > 0, os_get_proc_virtual_size() > 0, os_get_emulation_status(),
		       os_is_obs_plugin("whatever"), os_request_high_performance("x") == NULL);
		os_end_high_performance(NULL);
	} else if (IS("cpu_usage")) {
		os_cpu_usage_info_t *c = os_cpu_usage_info_start();
		for (volatile int i = 0; i < 2000000; i++)
			;
		double v = os_cpu_usage_info_query(c);
		double v0 = os_cpu_usage_info_query(NULL);
		printf("cpu_usage nonneg=%d null_is_zero=%d\n", v >= 0.0, v0 == 0.0);
		os_cpu_usage_info_destroy(c);
		os_cpu_usage_info_destroy(NULL);
	} else if (IS("uuid")) {
		char *a = os_generate_uuid(), *b = os_generate_uuid();
		bool fmt = strlen(a) == UUID_STR_LENGTH;
		for (int i = 0; fmt && i < UUID_STR_LENGTH; i++) {
			char c = a[i];
			if (i == 8 || i == 13 || i == 18 || i == 23)
				fmt = c == '-';
			else
				fmt = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
		}
		printf("uuid length=%zu format=%d version=%c variant_ok=%d distinct=%d\n", strlen(a), fmt, a[14],
		       a[19] == '8' || a[19] == '9' || a[19] == 'a' || a[19] == 'b', strcmp(a, b) != 0);
		bfree(a);
		bfree(b);
	} else if (IS("dl")) { /* NAME SYMBOL */
		script_need(sl, 3);
		void *m = os_dlopen(sl->argv[1]);
		printf("dl %s open=%d", sl->argv[1], m != NULL);
		if (m)
			printf(" sym=%d", os_dlsym(m, sl->argv[2]) != NULL);
		printf("\n");
		os_dlclose(m);
		os_dlclose(NULL);
	} else if (IS("dl_null")) {
		printf("dl_null %d\n", os_dlopen(NULL) == NULL);
	} else if (IS("try") || IS("try_defect")) {
		cmd_try(sl);
	} else {
		BAD(sl);
	}
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	base_set_log_handler(log_handler, NULL);
	base_set_crash_handler(crash_handler, NULL);

	const char *tmp = getenv("CONFORMANCE_TMPDIR");
	char top[PATH_MAX];
	snprintf(top, sizeof top, "%s", tmp && *tmp ? tmp : ".");
	snprintf(work, sizeof work, "%.4000s/platform", top);
	mkdir(work, 0755);
	if (!realpath(work, workr)) {
		perror("realpath");
		return 2;
	}
	empty_dir(work);
	if (chdir(work) != 0) {
		perror("chdir");
		return 2;
	}
	setenv("TZ", "UTC0", 1);
	tzset();
	base_ns = os_gettime_ns();

	struct script_line sl;
	int lineno = 0;
	while (script_next(f, &sl, &lineno))
		run(&sl);

	for (int i = 0; i < MAXH; i++)
		if (handles[i].f)
			fclose(handles[i].f);
	fclose(f);
	if (chdir(workr) == 0)
		empty_dir(workr);
	return 0;
}
