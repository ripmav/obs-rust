/*
 * Conformance harness: platform functions needed by file-serializer.c,
 * buffered-file-serializer.c (reproduced from platform.c / platform-nix.c).
 */
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>

int os_fseeki64(FILE *file, int64_t offset, int origin)
{
	return fseeko(file, (off_t)offset, origin);
}

int64_t os_ftelli64(FILE *file)
{
	return (int64_t)ftello(file);
}

int os_unlink(const char *path)
{
	return unlink(path);
}

int os_rename(const char *old_path, const char *new_path)
{
	return rename(old_path, new_path);
}
