/* Conformance swap-build stubs for the platform symbols referenced by the
 * Rust staticlib's cf-lexer bridge (cf_include_file -> os_fopen /
 * os_fread_utf8): when the runner references any staticlib symbol (e.g. the
 * P1-07.3 dbus/portal exports in swap mode) the linker pulls in the whole
 * Rust object, which references these. The utilrest runner never calls the
 * cf-lexer include path, so these must never run — abort() keeps a future
 * such call loud. In the real OBS_ENABLE_RUST build the definitions come
 * from platform-nix.c (not part of the swap set). */
#include <stdlib.h>

#include "util/platform.h"

FILE *os_fopen(const char *path, const char *mode)
{
	(void)path;
	(void)mode;
	abort();
}

uint64_t os_fread_utf8(FILE *file, char **pstr)
{
	(void)file;
	(void)pstr;
	abort();
}
