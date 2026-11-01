/*
 * Conformance harness: stand-ins for platform functions that the code under
 * test links against but that the conformance scripts never exercise. Any call
 * that reaches one of the abort() stubs means a script strayed outside the
 * area the harness covers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <wchar.h>
#include "util/bmem.h"

void os_breakpoint(void) {}
void os_oom(void) {}

FILE *os_fopen(const char *path, const char *mode)
{
	return fopen(path, mode);
}

size_t os_fread_utf8(FILE *file, char **pstr)
{
	*pstr = NULL;
	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	if (size > 0) {
		unsigned char bom[3] = {0, 0, 0};
		fseek(file, 0, SEEK_SET);
		if (fread(bom, 1, 3, file) != 3) { /* short file: no BOM */ }
		long offset = (bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF) ? 3 : 0;
		size -= offset;
		if (size == 0)
			return 0;
		char *buf = bmalloc((size_t)size + 1);
		fseek(file, offset, SEEK_SET);
		size_t n = fread(buf, 1, (size_t)size, file);
		if (n == 0) {
			bfree(buf);
			return 0;
		}
		buf[n] = 0;
		*pstr = buf;
	}
	return 0;
}

/* ASCII/UTF-8 only: the conformance scripts that reach these use plain ASCII. */
#include <string.h>
size_t utf8_to_wchar(const char *in, size_t insize, wchar_t *out, size_t outsize, int flags);

size_t os_mbs_to_utf8_ptr(const char *str, size_t len, char **dst)
{
	if (!len)
		len = strlen(str);
	*dst = malloc(len + 1);
	memcpy(*dst, str, len);
	(*dst)[len] = 0;
	return len;
}

size_t os_utf8_to_wcs_ptr(const char *str, size_t len, wchar_t **dst)
{
	size_t n = utf8_to_wchar(str, len, NULL, 0, 0);
	*dst = malloc((n + 1) * sizeof(wchar_t));
	utf8_to_wchar(str, len, *dst, n + 1, 0);
	(*dst)[n] = 0;
	return n;
}

/* ---- platform helpers used by config-file (behaviour reproduced from platform.c/-nix.c) ---- */
#include <errno.h>
#include <sys/stat.h>

/* Exact copy of the reference os_dtostr (platform.c), with from_locale() as a no-op for the C locale.
 * Note the reference defect: when the exponent is shortened the terminator is not moved, so the
 * buffer keeps stale characters ("1e+21" -> "1e211" while the return value says 4). */
int os_dtostr(double value, char *dst, size_t size)
{
	int ret;
	char *start, *end;
	size_t length;

	ret = snprintf(dst, size, "%.17g", value);
	if (ret < 0)
		return -1;

	length = (size_t)ret;
	if (length >= size)
		return -1;

	if (strchr(dst, '.') == NULL && strchr(dst, 'e') == NULL) {
		if (length + 3 >= size)
			return -1;
		dst[length] = '.';
		dst[length + 1] = '0';
		dst[length + 2] = '\0';
		length += 2;
	}

	start = strchr(dst, 'e');
	if (start) {
		start++;
		end = start + 1;
		if (*start == '-')
			start++;
		while (*end == '0')
			end++;
		if (end != start) {
			memmove(start, end, length - (size_t)(end - dst));
			length -= (size_t)(end - start);
		}
	}
	return (int)length;
}

double os_strtod(const char *str)
{
	char buf[64];
	strncpy(buf, str, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = 0;
	return strtod(buf, NULL);
}

bool os_file_exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

int os_safe_replace(const char *target, const char *from, const char *backup)
{
	if (backup && os_file_exists(target) && rename(target, backup) != 0)
		return -1;
	return rename(from, target);
}
