/*
 * Conformance harness (area props): platform functions that obs-data.c links
 * against but that the props scripts never exercise (file persistence).
 */
#include <stdio.h>

int os_rename(const char *old_path, const char *new_path)
{
	return rename(old_path, new_path);
}
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

char *os_quick_read_utf8_file(const char *path)
{
	(void)path;
	abort();
}

bool os_quick_write_utf8_file(const char *path, const char *str, size_t len, bool marker)
{
	(void)path, (void)str, (void)len, (void)marker;
	abort();
}

bool os_quick_write_utf8_file_safe(const char *path, const char *str, size_t len, bool marker, const char *temp_ext,
				   const char *backup_ext)
{
	(void)path, (void)str, (void)len, (void)marker, (void)temp_ext, (void)backup_ext;
	abort();
}
