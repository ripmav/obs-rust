// P1-02.4 (S-006): the four variadic dstr functions, which cannot be
// defined in Rust (the `va_list` ABI). The C implementation of dstr.c
// is kept verbatim here; they operate on the shared `struct dstr`
// (the dstr.h inlines drive the layout the Rust `Dstr` mirrors, the
// darray/deque precedent).
//
// Only swap builds compile this file: the conformance swap runner adds
// it when it links the Rust staticlib (run.sh; dstr.c itself is then
// dropped), and the OBS_ENABLE_RUST CMake build compiles it into
// libobs (the patch adds it to libobs/util/). Check builds compile
// dstr.c and never see this file.

#include "util/dstr.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void dstr_vprintf(struct dstr *dst, const char *format, va_list args)
{
	va_list args_cp;
	va_copy(args_cp, args);

	int len = vsnprintf(NULL, 0, format, args_cp);
	va_end(args_cp);

	if (len < 0)
		len = 4095;

	dstr_ensure_capacity(dst, ((size_t)len) + 1);
	len = vsnprintf(dst->array, ((size_t)len) + 1, format, args);

	if (!*dst->array) {
		dstr_free(dst);
		return;
	}

	dst->len = len < 0 ? strlen(dst->array) : (size_t)len;
}

void dstr_printf(struct dstr *dst, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	dstr_vprintf(dst, format, args);
	va_end(args);
}

void dstr_vcatf(struct dstr *dst, const char *format, va_list args)
{
	va_list args_cp;
	va_copy(args_cp, args);

	int len = vsnprintf(NULL, 0, format, args_cp);
	va_end(args_cp);

	if (len < 0)
		len = 4095;

	dstr_ensure_capacity(dst, dst->len + ((size_t)len) + 1);
	len = vsnprintf(dst->array + dst->len, ((size_t)len) + 1, format, args);

	if (!*dst->array) {
		dstr_free(dst);
		return;
	}

	dst->len += len < 0 ? strlen(dst->array + dst->len) : (size_t)len;
}

void dstr_catf(struct dstr *dst, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	dstr_vcatf(dst, format, args);
	va_end(args);
}
