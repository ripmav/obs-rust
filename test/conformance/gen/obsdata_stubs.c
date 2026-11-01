/*
 * Conformance harness: platform functions that obs-data.c calls for file
 * access (spec S-014). The behaviour is reproduced from libobs util/platform.c
 * and platform-nix.c (POSIX build); everything else comes from gen/stubs.c.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "util/bmem.h"
#include "util/base.h"
#include "util/dstr.h"
#include "util/platform.h"

char *os_quick_read_utf8_file(const char *path)
{
	FILE *f = os_fopen(path, "rb");
	char *file_string = NULL;

	if (!f)
		return NULL;

	os_fread_utf8(f, &file_string);
	fclose(f);

	return file_string;
}

bool os_quick_write_utf8_file(const char *path, const char *str, size_t len, bool marker)
{
	FILE *f = os_fopen(path, "wb");
	if (!f)
		return false;

	if (marker) {
		if (fwrite("\xEF\xBB\xBF", 3, 1, f) != 1) {
			fclose(f);
			return false;
		}
	}

	if (len) {
		if (fwrite(str, len, 1, f) != 1) {
			fclose(f);
			return false;
		}
	}
	fflush(f);
	fclose(f);

	return true;
}

bool os_quick_write_utf8_file_safe(const char *path, const char *str, size_t len, bool marker, const char *temp_ext,
				   const char *backup_ext)
{
	struct dstr backup_path = {0};
	struct dstr temp_path = {0};
	bool success = false;

	if (!temp_ext || !*temp_ext) {
		blog(LOG_ERROR, "os_quick_write_utf8_file_safe: invalid "
				"temporary extension specified");
		return false;
	}

	dstr_copy(&temp_path, path);
	if (*temp_ext != '.')
		dstr_cat(&temp_path, ".");
	dstr_cat(&temp_path, temp_ext);

	if (!os_quick_write_utf8_file(temp_path.array, str, len, marker)) {
		blog(LOG_ERROR,
		     "os_quick_write_utf8_file_safe: failed to "
		     "write to %s",
		     strstr(temp_path.array, "obsdata-f-") ? strstr(temp_path.array, "obsdata-f-") : temp_path.array);
		goto cleanup;
	}

	if (backup_ext && *backup_ext) {
		dstr_copy(&backup_path, path);
		if (*backup_ext != '.')
			dstr_cat(&backup_path, ".");
		dstr_cat(&backup_path, backup_ext);
	}

	if (os_safe_replace(path, temp_path.array, backup_path.array) == 0)
		success = true;

cleanup:
	dstr_free(&backup_path);
	dstr_free(&temp_path);
	return success;
}

int os_rename(const char *old_path, const char *new_path)
{
	return rename(old_path, new_path);
}
