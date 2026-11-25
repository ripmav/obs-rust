/* P1-02.4 (WP-32): allocator + platform stand-ins for the C `dstr.c`
 * compiled into the fuzz harness.
 *
 * The bmem* wrappers use the system allocator: dstr's observable
 * behaviour (len/capacity/content) does not depend on the allocator, and
 * the C side must be freed with the same allocator that allocated it.
 *
 * The case ops (`dstr_to_lower`/`dstr_to_upper`) drive the real UTF-8 ↔
 * wide path: `wchar_to_utf8`/`utf8_to_wchar` come from the pinned
 * `libobs/util/utf8.c` (see `build.rs`), and `os_utf8_to_wcs_ptr` is the
 * verbatim `libobs/util/platform.c` implementation. The mbs entry points
 * are not driven and stay stubbed. */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <util/dstr.h>
#include <util/utf8.h>

void *bmalloc(size_t size)
{
	return malloc(size ? size : 1);
}

void *brealloc(void *ptr, size_t size)
{
	return realloc(ptr, size ? size : 1);
}

void bfree(void *ptr)
{
	free(ptr);
}

void *bmemdup(const void *ptr, size_t size)
{
	void *mem = malloc(size ? size : 1);
	if (mem && size)
		memcpy(mem, ptr, size);
	return mem;
}

/* Verbatim from libobs/util/platform.c — the real UTF-8 → wide path the
 * case ops drive (needs `utf8_to_wchar` from the pinned utf8.c). */
size_t os_utf8_to_wcs(const char *str, size_t len, wchar_t *dst, size_t dst_size)
{
	size_t in_len;
	size_t out_len;

	if (!str)
		return 0;

	in_len = len ? len : strlen(str);
	out_len = dst ? (dst_size - 1) : utf8_to_wchar(str, in_len, NULL, 0, 0);

	if (dst) {
		if (!dst_size)
			return 0;

		if (out_len)
			out_len = utf8_to_wchar(str, in_len, dst, out_len + 1, 0);

		dst[out_len] = 0;
	}

	return out_len;
}

size_t os_utf8_to_wcs_ptr(const char *str, size_t len, wchar_t **pstr)
{
	if (str) {
		size_t out_len = os_utf8_to_wcs(str, len, NULL, 0);

		*pstr = bmalloc((out_len + 1) * sizeof(wchar_t));
		return os_utf8_to_wcs(str, len, *pstr, out_len + 1);
	} else {
		*pstr = NULL;
		return 0;
	}
}

size_t os_mbs_to_utf8_ptr(const char *str, size_t len, char **pstr)
{
	(void)str;
	(void)len;
	*pstr = NULL;
	return 0;
}

/* `wchar_to_utf8` comes from the pinned libobs/util/utf8.c (build.rs). */

/* Wrappers for the dstr.h `static inline` functions the target drives. */

void fz_dstr_init(struct dstr *dst)
{
	dstr_init(dst);
}

void fz_dstr_free(struct dstr *dst)
{
	dstr_free(dst);
}

void fz_dstr_reserve(struct dstr *dst, size_t capacity)
{
	dstr_reserve(dst, capacity);
}

void fz_dstr_resize(struct dstr *dst, size_t num)
{
	dstr_resize(dst, num);
}

void fz_dstr_ensure_capacity(struct dstr *dst, size_t new_size)
{
	dstr_ensure_capacity(dst, new_size);
}

void fz_dstr_cat(struct dstr *dst, const char *array)
{
	dstr_cat(dst, array);
}

void fz_dstr_cat_ch(struct dstr *dst, char ch)
{
	dstr_cat_ch(dst, ch);
}
