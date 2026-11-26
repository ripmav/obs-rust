/* WP-38 CI debug (temporary): the same call sequence in C (control for the
 * Rust raw-extern repro). Build: cl /nologo /W3 /Fe:repro-c.exe repro.c */
#include <windows.h>
#include <stdio.h>

/* SetThreadDescription / GetThreadDescription live in kernelbase.dll */
#pragma comment(lib, "kernelbase.lib")

int main(void) {
	printf("=== C repro ===\n");
	fflush(stdout);

	/* thread name */
	HMODULE m = LoadLibraryA("KernelBase.dll");
	printf("module=%p\n", (void *)m);
	fflush(stdout);
	FARPROC p = GetProcAddress(m, "SetThreadDescription");
	printf("proc=%p\n", (void *)p);
	fflush(stdout);
	if (p) {
		HRESULT hr =
			((HRESULT(WINAPI *)(HANDLE, PCWSTR))p)(GetCurrentThread(), L"hello");
		printf("SetThreadDescription hr=%ld\n", (long)hr);
	}
	fflush(stdout);
	PWSTR desc = NULL;
	HRESULT hr2 = GetThreadDescription(GetCurrentThread(), &desc);
	printf("GetThreadDescription hr=%ld desc=%p\n", (long)hr2, (void *)desc);
	if (desc) {
		printf("name=%ls\n", desc);
		LocalFree(desc);
	}
	fflush(stdout);
	if (m)
		FreeLibrary(m);

	/* pipes */
	SECURITY_ATTRIBUTES sa = {0};
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;
	HANDLE in1, out1, in2, out2;
	int r1 = (int)CreatePipe(&in1, &out1, &sa, 0);
	printf("CreatePipe1 ret=%d r=%p w=%p\n", r1, (void *)in1, (void *)out1);
	fflush(stdout);
	int r2 = (int)CreatePipe(&in2, &out2, &sa, 0);
	printf("CreatePipe2 ret=%d r=%p w=%p\n", r2, (void *)in2, (void *)out2);
	fflush(stdout);
	SetHandleInformation(in1, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(in2, HANDLE_FLAG_INHERIT, 0);

	STARTUPINFO si;
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES | STARTF_FORCEOFFFEEDBACK;
	si.hStdInput = NULL;
	si.hStdOutput = out1;
	si.hStdError = out2;
	PROCESS_INFORMATION pi;
	memset(&pi, 0, sizeof(pi));
	WCHAR cmdline[] = L"cmd /c echo hello";
	printf("about to CreateProcessW\n");
	fflush(stdout);
	BOOL ok = CreateProcessW(NULL, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW,
							 NULL, NULL, &si, &pi);
	printf("CreateProcessW ret=%d err=%lu pid=%lu\n", (int)ok,
		   (unsigned long)GetLastError(), (unsigned long)pi.dwProcessId);
	fflush(stdout);
	if (ok) {
		CloseHandle(pi.hThread);
		CloseHandle(out1);
		CloseHandle(out2);
		char buf[100];
		DWORD n = 0;
		BOOL rr = ReadFile(in1, buf, sizeof(buf), &n, NULL);
		buf[n < sizeof(buf) - 1 ? n : sizeof(buf) - 1] = 0;
		printf("ReadFile ret=%d bytes=%lu out=%s\n", (int)rr, (unsigned long)n,
			   buf);
		fflush(stdout);
		CloseHandle(in1);
		CloseHandle(in2);
		WaitForSingleObject(pi.hProcess, 10000);
		DWORD code = 0;
		GetExitCodeProcess(pi.hProcess, &code);
		printf("exit=%lu\n", (unsigned long)code);
		CloseHandle(pi.hProcess);
	}
	printf("=== C repro done ===\n");
	return 0;
}
