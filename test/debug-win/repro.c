/* WP-38 CI debug (temporary): bisection control for the Rust raw-extern
 * CreateProcessW failure. Variants V1..V6, each printing GetLastError.
 * Build: cl /nologo /W3 /Fe:repro-c.exe repro.c */
#include <windows.h>
#include <stdio.h>

static void attempt(const char *tag, LPCWSTR app, LPWSTR cmd, BOOL inherit,
				 DWORD flags, STARTUPINFO *si, PROCESS_INFORMATION *pi,
					 BOOL expect) {
	BOOL ok = CreateProcessW(app, cmd, NULL, NULL, inherit, flags, NULL, NULL,
							 si, pi);
	DWORD err = GetLastError();
	printf("%s ok=%d err=%lu pid=%lu\n", tag, (int)ok, (unsigned long)err,
		   (unsigned long)pi->dwProcessId);
	fflush(stdout);
	if (ok) {
		WaitForSingleObject(pi->hProcess, 10000);
		DWORD code = 0;
		GetExitCodeProcess(pi->hProcess, &code);
		printf("%s exit=%lu\n", tag, (unsigned long)code);
		CloseHandle(pi->hThread);
		CloseHandle(pi->hProcess);
	}
	fflush(stdout);
}

int main(void) {
	printf("=== C repro (bisection) ===\n");
	fflush(stdout);

	SECURITY_ATTRIBUTES sa = {0};
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;
	HANDLE in1, out1, in2, out2;
	int r1 = (int)CreatePipe(&in1, &out1, &sa, 0);
	int r2 = (int)CreatePipe(&in2, &out2, &sa, 0);
	printf("CreatePipe %d %d\n", r1, r2);
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
	WCHAR c1[] = L"cmd /c echo v1";
	WCHAR c2[] = L"cmd /c echo v2";
	WCHAR c3[] = L"cmd /c echo v3";
	WCHAR c4[] = L"cmd /c echo v4";
	WCHAR c5[] = L"cmd /c echo v5";
	WCHAR c6[] = L"cmd /c echo v6";
	WCHAR app[] = L"cmd";

	/* V1: ANSI CreateProcessA, no si (control: the old classic call) */
	{
		STARTUPINFOA s;
		memset(&s, 0, sizeof(s));
		s.cb = sizeof(s);
		PROCESS_INFORMATION p;
		BOOL ok =
			CreateProcessA(NULL, "cmd /c echo v1a", NULL, NULL, TRUE,
						   CREATE_NO_WINDOW, NULL, NULL, &s, &p);
		printf("V1  CreateProcessA+siA ok=%d err=%lu\n", (int)ok,
			   (unsigned long)GetLastError());
		if (ok) {
			WaitForSingleObject(p.hProcess, 10000);
			CloseHandle(p.hThread);
			CloseHandle(p.hProcess);
		}
		fflush(stdout);
	}

	/* V2: W, si = NULL, inherit = TRUE */
	memset(&pi, 0, sizeof(pi));
	attempt("V2  CreateProcessW si=NULL        ", NULL, c2, TRUE,
		   CREATE_NO_WINDOW, NULL, &pi, TRUE);
	/* V3: W, si with USESTDHANDLES + pipe handles */
	memset(&pi, 0, sizeof(pi));
	attempt("V3  CreateProcessW si+pipes       ", NULL, c3, TRUE,
		   CREATE_NO_WINDOW, &si, &pi, TRUE);
	/* V4: W, si with cb only (no flags), pipe handles set */
	{
		STARTUPINFO si2;
		memset(&si2, 0, sizeof(si2));
		si2.cb = sizeof(si2);
		si2.hStdOutput = out1;
		memset(&pi, 0, sizeof(pi));
		BOOL ok = CreateProcessW(NULL, c4, NULL, NULL, TRUE, CREATE_NO_WINDOW,
								 NULL, NULL, &si2, &pi);
		printf("V4  CreateProcessW si(cb only)     ok=%d err=%lu\n", (int)ok,
			   (unsigned long)GetLastError());
		if (ok) {
			WaitForSingleObject(pi.hProcess, 10000);
			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
		}
		fflush(stdout);
	}
	/* V5: W, si+pipes, lpApplicationName = L"cmd" */
	memset(&pi, 0, sizeof(pi));
	attempt("V5  CreateProcessW app=L\"cmd\"    ", app, c5, TRUE,
		   CREATE_NO_WINDOW, &si, &pi, TRUE);
	/* V6: W, si+pipes, creation flags = 0 (no CREATE_NO_WINDOW) */
	memset(&pi, 0, sizeof(pi));
	attempt("V6  CreateProcessW flags=0         ", NULL, c6, TRUE, 0, &si, &pi,
		   TRUE);

	printf("=== C repro done ===\n");
	return 0;
}
