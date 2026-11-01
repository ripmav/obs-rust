/*
 * Linux shim of the few Win32 declarations used by the win-capture / win-dshow code the wincapture runner
 * compiles unmodified (libobs/util/windows/window-helpers.c, win-dshow/shared-memory-queue.c, hook headers).
 * The behaviour behind these functions (fake window table, fake named file mappings) is in wincapture_runner.c.
 * Test tooling only.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>

typedef uint32_t DWORD;
typedef unsigned long ULONG_PTR_DUMMY;
typedef int BOOL;
typedef int HRESULT;
typedef void *HANDLE;
typedef void *HWND;
typedef void *HMODULE;
typedef wchar_t *LPWSTR;
typedef intptr_t LPARAM;
typedef intptr_t LONG_PTR;
typedef long LONG;
typedef uint32_t UINT;
typedef int (*WNDENUMPROC)(HWND, LPARAM);

#define WINAPI
#define CALLBACK
#define MAX_PATH 260
#define TRUE 1
#define FALSE 0
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#define SUCCEEDED(hr) ((hr) >= 0)
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define WS_CHILD 0x40000000L
#define WS_EX_TOOLWINDOW 0x00000080L
#define GW_HWNDNEXT 2
#define GW_CHILD 5
#define DWMWA_CLOAKED 14
#define FILE_MAP_READ 4
#define FILE_MAP_WRITE 2
#define FILE_MAP_ALL_ACCESS 0xF001F
#define PAGE_READWRITE 4
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define EVENT_MODIFY_STATE 2
#define SYNCHRONIZE 0x100000
#define WAIT_OBJECT_0 0
#define _snwprintf swprintf

typedef struct {
	LONG left, top, right, bottom;
} RECT;

HANDLE OpenProcess(DWORD, BOOL, DWORD);
BOOL CloseHandle(HANDLE);
DWORD GetCurrentProcessId(void);
DWORD GetWindowThreadProcessId(HWND, DWORD *);
DWORD GetProcessImageFileNameW(HANDLE, LPWSTR, DWORD);
int GetWindowTextLengthW(HWND);
int GetWindowTextW(HWND, LPWSTR, int);
int GetClassNameW(HWND, LPWSTR, int);
HMODULE GetModuleHandleA(const char *);
HWND GetDesktopWindow(void);
HWND FindWindowEx(HWND, HWND, const char *, const char *);
HWND GetWindow(HWND, UINT);
HWND GetNextWindow(HWND, UINT);
BOOL IsWindowVisible(HWND);
BOOL IsIconic(HWND);
BOOL GetClientRect(HWND, RECT *);
LONG_PTR GetWindowLongPtr(HWND, int);
HRESULT DwmGetWindowAttribute(HWND, DWORD, void *, DWORD);
BOOL EnumWindows(WNDENUMPROC, LPARAM);

HANDLE CreateFileMappingW(HANDLE, void *, DWORD, DWORD, DWORD, const wchar_t *);
HANDLE OpenFileMappingW(DWORD, BOOL, const wchar_t *);
void *MapViewOfFile(HANDLE, DWORD, DWORD, DWORD, size_t);
BOOL UnmapViewOfFile(void *);
HANDLE CreateEventW(void *, BOOL, BOOL, const wchar_t *);
HANDLE OpenEventW(DWORD, BOOL, const wchar_t *);
HANDLE CreateMutexW(void *, BOOL, const wchar_t *);
HANDLE OpenMutexW(DWORD, BOOL, const wchar_t *);
DWORD WaitForSingleObject(HANDLE, DWORD);
