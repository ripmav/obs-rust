// GetSockError() of the reference (shared/happy-eyeballs/happy-eyeballs.c):
// WSAGetLastError() on Windows, errno elsewhere. Read in C (not Rust)
// because the errno accessor symbol differs per ABI/OS version (glibc
// __errno_location, musl __errno, the macOS CRT's per-version accessor);
// the C errno macro resolves the right one everywhere. The happy-eyeballs
// bridge calls obs_ffi_get_sock_error() instead of per-ABI Rust externs.
#ifdef _WIN32
#include <winsock2.h>

int obs_ffi_get_sock_error(void)
{
	return WSAGetLastError();
}
#else
#include <errno.h>

int obs_ffi_get_sock_error(void)
{
	return (int)errno;
}
#endif
