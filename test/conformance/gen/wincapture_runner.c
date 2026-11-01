/*
 * Conformance runner for spec S-051 (Windows capture plugins: win-capture, win-wasapi, win-dshow).
 *
 * REAL C (compiled unmodified on Linux against gen/wincapture_shim, a fake Win32 layer):
 *   - libobs/util/windows/window-helpers.c    window enumeration, filtering, rating, window-string encoding
 *   - shared/obs-hook-config headers          hook_info / offsets layouts, object names, create_hook_info()
 *   - plugins/win-dshow/shared-memory-queue.c virtual camera frame queue (+ tiny-nv12-scale.c)
 * MODEL (hand-ported from the reference source because the originals are static C++/C in files that cannot be
 * compiled here): every command whose name starts with m_ . Their expected output is source-derived, not
 * produced by the reference binary.
 * Script language: see test/conformance/README.md, section "wincapture runner (spec S-051)".
 */
#include <limits.h>
#include <math.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>
#include <stddef.h>
#include "harness.h"
#include <Windows.h>
#include <obs-properties.h>
#include <obs-data.h>
#include <util/dstr.h>

#include "graphics-hook-info.h"
#include "graphics-hook-ver.h"
#include <util/util_uint64.h>

/* ------------------------------------------------------------------ fake settings store (obs_data_t) */

#define MAXKV 48
struct kv {
	char key[48];
	char type; /* i b s */
	long long i;
	char s[600];
	bool user;
};
struct obs_data {
	struct kv kv[MAXKV];
	int n;
};
static struct obs_data S;

static struct kv *kv_find(const char *key)
{
	for (int i = 0; i < S.n; i++)
		if (!strcmp(S.kv[i].key, key))
			return &S.kv[i];
	return NULL;
}
static void kv_put(const char *key, char type, const char *val, bool user)
{
	struct kv *k = kv_find(key);
	if (!k) {
		k = &S.kv[S.n++];
		memset(k, 0, sizeof(*k));
		snprintf(k->key, sizeof(k->key), "%s", key);
		k->type = type;
	}
	if (user)
		k->user = true;
	else if (k->user)
		return; /* a default never overrides a user value */
	k->type = type;
	k->i = type == 's' ? 0 : strtoll(!strcmp(val, "true") ? "1" : !strcmp(val, "false") ? "0" : val, NULL, 10);
	snprintf(k->s, sizeof(k->s), "%s", val);
}
const char *obs_data_get_string(obs_data_t *d, const char *name)
{
	(void)d;
	struct kv *k = kv_find(name);
	return k && k->type == 's' ? k->s : "";
}
long long obs_data_get_int(obs_data_t *d, const char *name)
{
	(void)d;
	struct kv *k = kv_find(name);
	return k && k->type != 's' ? k->i : 0;
}
bool obs_data_get_bool(obs_data_t *d, const char *name)
{
	return obs_data_get_int(d, name) != 0;
}
bool obs_data_has_user_value(obs_data_t *d, const char *name)
{
	(void)d;
	struct kv *k = kv_find(name);
	return k && k->user;
}

/* ------------------------------------------------------------------ fake obs_property (list) */

struct obs_property {
	int dummy;
};
static struct obs_property g_prop;
struct pitem {
	char name[700];
	char val[700];
	bool disabled;
};
static struct pitem plist[48];
static size_t pn;

size_t obs_property_list_add_string(obs_property_t *p, const char *name, const char *val)
{
	(void)p;
	snprintf(plist[pn].name, sizeof(plist[pn].name), "%s", name);
	snprintf(plist[pn].val, sizeof(plist[pn].val), "%s", val);
	plist[pn].disabled = false;
	return pn++;
}
void obs_property_list_insert_string(obs_property_t *p, size_t idx, const char *name, const char *val)
{
	(void)p;
	for (size_t i = pn; i > idx; i--)
		plist[i] = plist[i - 1];
	snprintf(plist[idx].name, sizeof(plist[idx].name), "%s", name);
	snprintf(plist[idx].val, sizeof(plist[idx].val), "%s", val);
	plist[idx].disabled = false;
	pn++;
}
void obs_property_list_item_disable(obs_property_t *p, size_t idx, bool disabled)
{
	(void)p;
	plist[idx].disabled = disabled;
}
const char *obs_property_list_item_string(obs_property_t *p, size_t idx)
{
	(void)p;
	return idx < pn ? plist[idx].val : NULL;
}

/* ------------------------------------------------------------------ fake windows */

#define DESKTOP ((HWND)(intptr_t)1000)
#define SELF_PID 4242
struct fwin {
	int id, parent; /* parent 0 = top level */
	char title[2200], cls[300], exe[300];
	DWORD pid;
	bool visible, iconic, cloaked, tool, child;
	int cw, ch;
};
static struct fwin W[40];
static int nw;
static bool findex_broken;

static struct fwin *wget(HWND h)
{
	for (int i = 0; i < nw; i++)
		if ((HWND)(intptr_t)W[i].id == h)
			return &W[i];
	return NULL;
}
static size_t to_w(const char *s, wchar_t *out, size_t cap)
{
	size_t n = 0;
	for (; s[n] && n + 1 < cap; n++)
		out[n] = (unsigned char)s[n];
	out[n] = 0;
	return n;
}

HANDLE OpenProcess(DWORD a, BOOL b, DWORD pid)
{
	(void)a, (void)b;
	for (int i = 0; i < nw; i++)
		if (W[i].pid == pid)
			return !strcmp(W[i].exe, "!noaccess") ? NULL : (HANDLE)(intptr_t)pid;
	return NULL;
}
void *ms_get_obfuscated_func(HMODULE m, const char *s, uint64_t v)
{
	(void)m, (void)s, (void)v;
	return (void *)OpenProcess;
}
HMODULE GetModuleHandleA(const char *n)
{
	(void)n;
	return (HMODULE)1;
}
DWORD GetCurrentProcessId(void)
{
	return SELF_PID;
}
DWORD GetWindowThreadProcessId(HWND h, DWORD *pid)
{
	struct fwin *w = wget(h);
	*pid = w ? w->pid : 0;
	return w ? 1 : 0;
}
DWORD GetProcessImageFileNameW(HANDLE p, LPWSTR out, DWORD cap)
{
	DWORD pid = (DWORD)(intptr_t)p;
	for (int i = 0; i < nw; i++)
		if (W[i].pid == pid) {
			if (!strcmp(W[i].exe, "!nopath"))
				return 0;
			return (DWORD)to_w(W[i].exe, out, cap);
		}
	return 0;
}
int GetWindowTextLengthW(HWND h)
{
	struct fwin *w = wget(h);
	return w ? (int)strlen(w->title) : 0;
}
int GetWindowTextW(HWND h, LPWSTR out, int cap)
{
	struct fwin *w = wget(h);
	return w ? (int)to_w(w->title, out, (size_t)cap) : 0;
}
int GetClassNameW(HWND h, LPWSTR out, int cap)
{
	struct fwin *w = wget(h);
	return w ? (int)to_w(w->cls, out, (size_t)cap) : 0;
}
HWND GetDesktopWindow(void)
{
	return DESKTOP;
}
static HWND sibling_after(int parent, HWND prev)
{
	bool seen = prev == NULL;
	for (int i = 0; i < nw; i++) {
		if (W[i].parent != parent)
			continue;
		if (seen)
			return (HWND)(intptr_t)W[i].id;
		if ((HWND)(intptr_t)W[i].id == prev)
			seen = true;
	}
	return NULL;
}
HWND FindWindowEx(HWND parent, HWND prev, const char *c, const char *t)
{
	(void)c, (void)t;
	if (parent == DESKTOP)
		return findex_broken ? NULL : sibling_after(0, prev);
	struct fwin *p = wget(parent);
	return p ? sibling_after(p->id, prev) : NULL;
}
HWND GetWindow(HWND h, UINT cmd)
{
	return (h == DESKTOP && cmd == GW_CHILD) ? sibling_after(0, NULL) : NULL;
}
HWND GetNextWindow(HWND h, UINT cmd)
{
	struct fwin *w = wget(h);
	return (w && cmd == GW_HWNDNEXT) ? sibling_after(w->parent, h) : NULL;
}
BOOL IsWindowVisible(HWND h)
{
	struct fwin *w = wget(h);
	return w && w->visible;
}
BOOL IsIconic(HWND h)
{
	struct fwin *w = wget(h);
	return w && w->iconic;
}
BOOL GetClientRect(HWND h, RECT *r)
{
	struct fwin *w = wget(h);
	memset(r, 0, sizeof(*r));
	if (!w)
		return 0;
	r->right = w->cw;
	r->bottom = w->ch;
	return 1;
}
LONG_PTR GetWindowLongPtr(HWND h, int idx)
{
	struct fwin *w = wget(h);
	if (!w)
		return 0;
	return idx == GWL_STYLE ? (w->child ? WS_CHILD : 0) : (w->tool ? WS_EX_TOOLWINDOW : 0);
}
HRESULT DwmGetWindowAttribute(HWND h, DWORD a, void *out, DWORD sz)
{
	(void)a, (void)sz;
	struct fwin *w = wget(h);
	*(DWORD *)out = w && w->cloaked;
	return 0;
}
BOOL EnumWindows(WNDENUMPROC cb, LPARAM lp)
{
	for (int i = 0; i < nw; i++)
		if (W[i].parent == 0 && !cb((HWND)(intptr_t)W[i].id, lp))
			break;
	return 1;
}

/* ------------------------------------------------------------------ fake named mappings / events */

struct fmap {
	wchar_t name[96];
	uint8_t *buf;
	size_t size;
	int refs;
};
static struct fmap M[8];
static int wlen(const wchar_t *s)
{
	return (int)wcslen(s);
}
static struct fmap *mfind(const wchar_t *n)
{
	for (int i = 0; i < 8; i++)
		if (M[i].buf && !wcscmp(M[i].name, n))
			return &M[i];
	return NULL;
}
static char last_map_name[128];
HANDLE CreateFileMappingW(HANDLE f, void *a, DWORD p, DWORD hi, DWORD lo, const wchar_t *name)
{
	(void)f, (void)a, (void)p, (void)hi;
	for (int k = 0; k < wlen(name) && k < 120; k++)
		last_map_name[k] = (char)name[k], last_map_name[k + 1] = 0;
	struct fmap *m = mfind(name);
	if (m) {
		m->refs++;
		return m;
	}
	for (int i = 0; i < 8; i++)
		if (!M[i].buf) {
			wcsncpy(M[i].name, name, 95);
			M[i].buf = calloc(1, lo);
			M[i].size = lo;
			M[i].refs = 1;
			return &M[i];
		}
	return NULL;
}
HANDLE OpenFileMappingW(DWORD a, BOOL b, const wchar_t *name)
{
	(void)a, (void)b;
	struct fmap *m = mfind(name);
	if (m)
		m->refs++;
	return m;
}
void *MapViewOfFile(HANDLE h, DWORD a, DWORD b, DWORD c, size_t d)
{
	(void)a, (void)b, (void)c, (void)d;
	return ((struct fmap *)h)->buf;
}
BOOL UnmapViewOfFile(void *p)
{
	(void)p;
	return 1;
}
BOOL CloseHandle(HANDLE h)
{
	for (int i = 0; i < 8; i++)
		if (h == &M[i] && M[i].buf) {
			if (--M[i].refs == 0) {
				free(M[i].buf);
				M[i].buf = NULL;
			}
			return 1;
		}
	return 1;
}

/* ------------------------------------------------------------------ real code under test */

/* --- stubs for compat-helpers.c: compatibility.json is looked up as $CONFORMANCE_TMPDIR/wincapture-compat.json */
#include <obs-module.h>
#include <util/platform.h>
static char compat_path[512];
static struct {
	char key[100], text[300];
} compat_tr[8];
static int ncompat_tr;
obs_module_t *obs_current_module(void)
{
	return NULL;
}
char *obs_module_get_config_path(obs_module_t *m, const char *file)
{
	(void)m, (void)file;
	return NULL;
}
char *obs_find_module_file(obs_module_t *m, const char *file)
{
	(void)m, (void)file;
	FILE *fp = fopen(compat_path, "rb");
	if (!fp)
		return NULL;
	fclose(fp);
	return bstrdup(compat_path);
}
bool obs_module_get_string(const char *key, const char **out)
{
	for (int i = 0; i < ncompat_tr; i++)
		if (key && !strcmp(compat_tr[i].key, key)) {
			*out = compat_tr[i].text;
			return true;
		}
	return false;
}
char *os_quick_read_utf8_file(const char *path)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return NULL;
	char *buf = bmalloc(65536);
	size_t n = fread(buf, 1, 65535, fp);
	buf[n] = 0;
	fclose(fp);
	return buf;
}
#include "../../../plugins/win-capture/compat-helpers.c"
#include "../../../libobs/util/windows/window-helpers.c"
#include "../../../plugins/win-dshow/shared-memory-queue.c"

/* ------------------------------------------------------------------ script helpers */

static const char *arg(struct script_line *sl, int i)
{
	if (i >= sl->argc)
		return "-";
	return sl->argv[i];
}
static const char *nz(const char *s)
{
	return !strcmp(s, "-") ? NULL : s;
}
static long long num(struct script_line *sl, int i)
{
	return strtoll(arg(sl, i), NULL, 0);
}
static void ps(const char *label, const char *s)
{
	printf("%s %s\n", label, s ? s : "(null)");
}
static void pstr(const char *label, const char *s)
{
	if (!s) {
		printf("%s (null)\n", label);
		return;
	}
	printf("%s \"", label);
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (*p == '"' || *p == '\\')
			printf("\\%c", *p);
		else if (*p < 32 || *p > 126)
			printf("\\x%02x", *p);
		else
			putchar(*p);
	}
	printf("\"\n");
}
static void hex(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		printf("%02x", p[i]);
	printf("\n");
}
static const char *prio_name(int p)
{
	return p == WINDOW_PRIORITY_CLASS ? "class" : p == WINDOW_PRIORITY_TITLE ? "title" : "exe";
}
static int parse_prio(const char *s)
{
	return !strcmp(s, "class") ? WINDOW_PRIORITY_CLASS : !strcmp(s, "title") ? WINDOW_PRIORITY_TITLE
									       : WINDOW_PRIORITY_EXE;
}
static void print_hwnd(const char *label, HWND h)
{
	if (h)
		printf("%s %d\n", label, (int)(intptr_t)h);
	else
		printf("%s none\n", label);
}

/* ------------------------------------------------------------------ MODEL: window capture / game capture / monitor */

static int m_wc_method(int method, bool wgc, const char *cls)
{
	static const char *partial[] = {"Chrome", "Mozilla", NULL};
	static const char *whole[] = {"ApplicationFrameWindow",
				      "Windows.UI.Core.CoreWindow",
				      "WinUIDesktopWin32WindowClass",
				      "GAMINGSERVICESUI_HOSTING_WINDOW_CLASS",
				      "XLMAIN",
				      "PPTFrameClass",
				      "screenClass",
				      "PodiumParent",
				      "OpusApp",
				      "OMain",
				      "Framework::CFrame",
				      "rctrl_renwnd32",
				      "MSWinPub",
				      "OfficeApp-Frame",
				      "WindowsWebview2Wrapper",
				      "SDL_app",
				      "WINDOWSCLIENT",
				      NULL};
	if (!wgc)
		return 1; /* BITBLT */
	if (method != 0)
		return method;
	if (!cls)
		return 1;
	for (const char **c = partial; *c; c++)
		if (astrstri(cls, *c))
			return 2;
	for (const char **c = whole; *c; c++)
		if (astrcmpi(cls, *c) == 0)
			return 2;
	return 1;
}
static const char *method_name(int m)
{
	return m == 0 ? "auto" : m == 1 ? "bitblt" : m == 2 ? "wgc" : "?";
}
static void defaults_load(const char *kind)
{
#define DI(k, v) kv_put(k, 'i', #v, false)
#define DB(k, v) kv_put(k, 'b', #v, false)
#define DS(k, v) kv_put(k, 's', v, false)
	if (!strcmp(kind, "window_capture")) {
		DI("method", 0), DB("cursor", true), DB("force_sdr", false), DB("compatibility", false),
			DB("client_area", true);
	} else if (!strcmp(kind, "game_capture")) {
		DS("capture_mode", "any_fullscreen"), DI("priority", 2), DB("sli_compatibility", false),
			DB("capture_cursor", true), DB("allow_transparency", false), DB("premultiplied_alpha", false),
			DB("limit_framerate", false), DB("capture_overlays", false), DB("anti_cheat_hook", true),
			DI("hook_rate", 1), DS("rgb10a2_space", "srgb");
	} else if (!strcmp(kind, "monitor_capture")) {
		DI("monitor", 0), DB("capture_cursor", true), DB("compatibility", false);
	} else if (!strcmp(kind, "display_capture")) {
		DI("method", 0), DS("monitor_id", "DUMMY"), DI("monitor_wgc", 0), DB("capture_cursor", true),
			DB("force_sdr", false);
	} else if (!strcmp(kind, "wasapi_input")) {
		DS("device_id", "default"), DB("use_device_timing", false);
	} else if (!strcmp(kind, "wasapi_output")) {
		DS("device_id", "default"), DB("use_device_timing", true);
	} else if (!strcmp(kind, "wasapi_process")) {
		/* no defaults */
	} else if (!strcmp(kind, "dshow")) {
		DI("frame_interval", -1), DI("res_type", 0), DI("video_format", -1 /* VideoFormat::Any, see spec Q */),
			DB("active", true), DS("color_space", "default"), DS("color_range", "default"),
			DI("audio_output_mode", 0), DB("autorotation", true), DB("hw_decode", false);
	} else if (!strcmp(kind, "dshow_encoder")) {
		DI("bitrate", 1000);
	}
}

/* ------------------------------------------------------------------ MODEL: WASAPI */

enum { SP_UNKNOWN = 0, SP_MONO, SP_STEREO, SP_2P1, SP_4P0, SP_4P1, SP_5P1, SP_7P1 = 8 };
#define KS_2P1 0x0B
#define KS_SURR 0x107
#define KS_4P1 0x10F
#define KS_5P1S 0x60F
#define KS_7P1S 0x63F
#define KS_STEREO 0x03
static int m_ks_to_layout(uint32_t mask, uint16_t channels)
{
	switch (mask) {
	case KS_2P1:
		return SP_2P1;
	case KS_SURR:
		return SP_4P0;
	case KS_4P1:
		return SP_4P1;
	case KS_5P1S:
		return SP_5P1;
	case KS_7P1S:
		return SP_7P1;
	}
	return channels;
}
static uint32_t m_layout_to_ks(int l)
{
	switch (l) {
	case SP_STEREO:
		return KS_STEREO;
	case SP_2P1:
		return KS_2P1;
	case SP_4P0:
		return KS_SURR;
	case SP_4P1:
		return KS_4P1;
	case SP_5P1:
		return KS_5P1S;
	case SP_7P1:
		return KS_7P1S;
	}
	return (uint32_t)l;
}
struct wsrc {
	int type; /* 0 input 1 device-output 2 process-output */
	bool active;
	char device_id[300], cls[300], title[300], exe[300];
	int priority;
	bool timing, is_default;
	char default_id[100];
};
static struct wsrc WS;
static void wasapi_params(struct wsrc *dst)
{
	snprintf(dst->device_id, sizeof(dst->device_id), "%s", obs_data_get_string(&S, "device_id"));
	dst->timing = obs_data_get_bool(&S, "use_device_timing");
	dst->is_default = strcasecmp(dst->device_id, "default") == 0;
	dst->priority = (int)obs_data_get_int(&S, "priority");
	dst->cls[0] = dst->title[0] = dst->exe[0] = 0;
	if (dst->type != 0) {
		char *c, *t, *e;
		ms_build_window_strings(obs_data_get_string(&S, "window"), &c, &t, &e);
		if (c)
			snprintf(dst->cls, sizeof(dst->cls), "%s", c);
		if (t)
			snprintf(dst->title, sizeof(dst->title), "%s", t);
		if (e)
			snprintf(dst->exe, sizeof(dst->exe), "%s", e);
		bfree(c), bfree(t), bfree(e);
	}
}

/* ------------------------------------------------------------------ MODEL: dshow */

#define FPS_HIGHEST 0LL
#define FPS_MATCHING -1LL
#define FMT_ANY (-1)
struct cap {
	int fmt, minCX, minCY, maxCX, maxCY;
	long long minI, maxI;
};
static struct cap CAPS[16];
static int ncaps;
static bool between(long long lo, long long v, long long hi)
{
	return v >= lo && v <= hi;
}
static bool fr_available(const struct cap *c, long long iv)
{
	return iv == FPS_HIGHEST || iv == FPS_MATCHING || between(c->minI - 20, iv, c->maxI + 20);
}
static long long fr_interval(const struct cap *c, long long want)
{
	return want < c->minI ? c->minI : (want < c->maxI ? want : c->maxI);
}
struct res {
	int cx, cy;
};
static struct res RES[64];
static int nres;
static void insert_resolution(int cx, int cy)
{
	int bestCY = 0;
	int idx = 0;
	for (; idx < nres; idx++) {
		if (RES[idx].cx > cx)
			break;
		if (RES[idx].cx == cx) {
			if (RES[idx].cy == cy)
				return;
			if (!bestCY)
				bestCY = RES[idx].cy;
			else if (RES[idx].cy > bestCY)
				break;
		}
	}
	for (int i = nres; i > idx; i--)
		RES[i] = RES[i - 1];
	RES[idx].cx = cx, RES[idx].cy = cy;
	nres++;
}
static const char *fmtname(int f)
{
	static char b[16];
	if (f == FMT_ANY)
		return "any";
	snprintf(b, sizeof b, "f%d", f);
	return b;
}

/* ------------------------------------------------------------------ vq state */

static video_queue_t *VQ[4];

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;
	bool wgc_supported = true;
	struct hook_info hi;
	memset(&hi, 0, sizeof(hi));

	while (script_next(f, &sl, &lineno)) {
		const char *c = sl.argv[0];
		/* ---------------------------------------------------------------- settings store */
		if (!strcmp(c, "s_reset")) {
			memset(&S, 0, sizeof(S));
		} else if (!strcmp(c, "s_defaults")) {
			defaults_load(arg(&sl, 1));
		} else if (!strcmp(c, "s_set")) { /* s_set key int|bool|str value */
			char t = !strcmp(arg(&sl, 2), "int") ? 'i' : !strcmp(arg(&sl, 2), "bool") ? 'b' : 's';
			kv_put(arg(&sl, 1), t, arg(&sl, 3), true);
		} else if (!strcmp(c, "s_dump")) {
			for (int i = 0; i < S.n; i++) {
				struct kv *k = &S.kv[i];
				char nb[32];
				snprintf(nb, sizeof nb, "%lld", k->i);
				printf("%s %s %s%s\n", k->key, k->type == 's' ? "str" : k->type == 'b' ? "bool" : "int",
				       k->type == 's' ? k->s : k->type == 'b' ? (k->i ? "true" : "false") : nb,
				       k->user ? " user" : " default");
			}
		} else if (!strcmp(c, "s_get")) {
			struct kv *k = kv_find(arg(&sl, 1));
			if (!k)
				printf("get %s missing\n", arg(&sl, 1));
			else if (k->type == 's')
				pstr(k->key, k->s);
			else
				printf("%s %lld\n", k->key, k->i);
		}
		/* ---------------------------------------------------------------- fake windows */
		else if (!strcmp(c, "win_reset")) {
			nw = 0;
			findex_broken = false;
			pn = 0;
		} else if (!strcmp(c, "win_fallback")) {
			findex_broken = num(&sl, 1) != 0;
		} else if (!strcmp(c, "win_add")) { /* win_add id title class exe pid [flag...] */
			struct fwin *w = &W[nw++];
			memset(w, 0, sizeof(*w));
			w->id = (int)num(&sl, 1);
			snprintf(w->title, sizeof(w->title), "%s", !strcmp(arg(&sl, 2), "-") ? "" : arg(&sl, 2));
			snprintf(w->cls, sizeof(w->cls), "%s", arg(&sl, 3));
			snprintf(w->exe, sizeof(w->exe), "%s", arg(&sl, 4));
			w->pid = (DWORD)num(&sl, 5);
			w->visible = true;
			w->cw = 800, w->ch = 600;
			for (int i = 6; i < sl.argc; i++) {
				const char *fl = sl.argv[i];
				if (!strcmp(fl, "hidden"))
					w->visible = false;
				else if (!strcmp(fl, "iconic"))
					w->iconic = true;
				else if (!strcmp(fl, "cloaked"))
					w->cloaked = true;
				else if (!strcmp(fl, "tool"))
					w->tool = true;
				else if (!strcmp(fl, "child"))
					w->child = true;
				else if (!strcmp(fl, "empty"))
					w->cw = 0;
				else if (!strncmp(fl, "parent=", 7))
					w->parent = atoi(fl + 7);
			}
		} else if (!strcmp(c, "win_info")) {
			int id = (int)num(&sl, 1);
			struct dstr t = {0}, k = {0}, e = {0};
			bool ok = ms_get_window_exe(&e, (HWND)(intptr_t)id);
			ms_get_window_title(&t, (HWND)(intptr_t)id);
			ms_get_window_class(&k, (HWND)(intptr_t)id);
			printf("win %d exe_ok=%d\n", id, ok);
			pstr("  exe", e.array);
			pstr("  title", t.array);
			pstr("  class", k.array);
			dstr_free(&t), dstr_free(&k), dstr_free(&e);
		} else if (!strcmp(c, "win_uwp")) {
			int id = (int)num(&sl, 1);
			printf("is_uwp %d %d\n", id, ms_is_uwp_window((HWND)(intptr_t)id));
			print_hwnd("uwp_actual", ms_get_uwp_actual_window((HWND)(intptr_t)id));
		} else if (!strcmp(c, "find") || !strcmp(c, "find_top")) { /* mode prio class title exe */
			enum window_search_mode mode = !strcmp(arg(&sl, 1), "exclude") ? EXCLUDE_MINIMIZED : INCLUDE_MINIMIZED;
			int prio = parse_prio(arg(&sl, 2));
			HWND h = !strcmp(c, "find") ? ms_find_window(mode, prio, nz(arg(&sl, 3)), nz(arg(&sl, 4)), nz(arg(&sl, 5)))
						    : ms_find_window_top_level(mode, prio, nz(arg(&sl, 3)), nz(arg(&sl, 4)),
									       nz(arg(&sl, 5)));
			print_hwnd(c, h);
		} else if (!strcmp(c, "rating")) { /* id prio class title exe */
			int id = (int)num(&sl, 1);
			const char *cls = arg(&sl, 3);
			int r = window_rating((HWND)(intptr_t)id, parse_prio(arg(&sl, 2)), cls, arg(&sl, 4), arg(&sl, 5),
					      is_uwp_class(cls), is_generic_class(cls));
			printf("rating %d %s = %d\n", id, prio_name(parse_prio(arg(&sl, 2))), r);
		} else if (!strcmp(c, "list")) { /* mode */
			pn = 0;
			ms_fill_window_list(&g_prop, !strcmp(arg(&sl, 1), "exclude") ? EXCLUDE_MINIMIZED : INCLUDE_MINIMIZED,
					    NULL);
			printf("list %zu items\n", pn);
			for (size_t i = 0; i < pn; i++) {
				pstr("  name", plist[i].name);
				pstr("  value", plist[i].val);
			}
		} else if (!strcmp(c, "prop_clear")) {
			pn = 0;
		} else if (!strcmp(c, "prop_add")) {
			obs_property_list_add_string(&g_prop, arg(&sl, 1), arg(&sl, 2));
		} else if (!strcmp(c, "prop_check")) { /* settings key already in store; key idx */
			bool r = ms_check_window_property_setting(NULL, &g_prop, &S, arg(&sl, 1), (size_t)num(&sl, 2));
			printf("check %d\n", r);
			for (size_t i = 0; i < pn; i++)
				printf("  [%zu] name=%s value=%s disabled=%d\n", i, plist[i].name, plist[i].val,
				       plist[i].disabled);
		} else if (!strcmp(c, "build_strings")) {
			char *k, *t, *e;
			ms_build_window_strings(nz(arg(&sl, 1)), &k, &t, &e);
			pstr("title", t);
			pstr("class", k);
			pstr("exe", e);
			bfree(k), bfree(t), bfree(e);
		} else if (!strcmp(c, "encode_decode")) { /* title class exe: encode each, join with ':', parse back */
			struct dstr a = {0}, b = {0}, d = {0}, enc = {0};
			dstr_copy(&a, arg(&sl, 1)), dstr_copy(&b, arg(&sl, 2)), dstr_copy(&d, arg(&sl, 3));
			encode_dstr(&a), encode_dstr(&b), encode_dstr(&d);
			dstr_cat_dstr(&enc, &a), dstr_cat(&enc, ":"), dstr_cat_dstr(&enc, &b), dstr_cat(&enc, ":"),
				dstr_cat_dstr(&enc, &d);
			pstr("encoded", enc.array);
			char *k, *t, *e;
			ms_build_window_strings(enc.array, &k, &t, &e);
			pstr("title", t);
			pstr("class", k);
			pstr("exe", e);
			bfree(k), bfree(t), bfree(e);
			dstr_free(&a), dstr_free(&b), dstr_free(&d), dstr_free(&enc);
		}
		/* ---------------------------------------------------------------- hook protocol (real headers) */
		else if (!strcmp(c, "hook_layout")) {
			printf("sizeof hook_info %zu\n", sizeof(struct hook_info));
			printf("sizeof graphics_offsets %zu\n", sizeof(struct graphics_offsets));
			printf("sizeof d3d8_offsets %zu\n", sizeof(struct d3d8_offsets));
			printf("sizeof d3d9_offsets %zu\n", sizeof(struct d3d9_offsets));
			printf("sizeof dxgi_offsets %zu\n", sizeof(struct dxgi_offsets));
			printf("sizeof ddraw_offsets %zu\n", sizeof(struct ddraw_offsets));
			printf("sizeof dxgi_offsets2 %zu\n", sizeof(struct dxgi_offsets2));
			printf("sizeof d3d12_offsets %zu\n", sizeof(struct d3d12_offsets));
			printf("sizeof shmem_data %zu\n", sizeof(struct shmem_data));
			printf("sizeof shtex_data %zu\n", sizeof(struct shtex_data));
#define OFF(m) printf("offset hook_info.%s %zu\n", #m, offsetof(struct hook_info, m))
			OFF(hook_ver_major), OFF(hook_ver_minor), OFF(type), OFF(window), OFF(format), OFF(cx), OFF(cy),
				OFF(UNUSED_base_cx), OFF(UNUSED_base_cy), OFF(pitch), OFF(map_id), OFF(map_size),
				OFF(flip), OFF(frame_interval), OFF(UNUSED_use_scale), OFF(force_shmem),
				OFF(capture_overlay), OFF(allow_srgb_alias), OFF(offsets), OFF(reserved);
#define OFF2(m) printf("offset graphics_offsets.%s %zu\n", #m, offsetof(struct graphics_offsets, m))
			OFF2(d3d8), OFF2(d3d9), OFF2(dxgi), OFF2(ddraw), OFF2(dxgi2), OFF2(d3d12);
			printf("offset shmem_data.last_tex %zu\n", offsetof(struct shmem_data, last_tex));
			printf("offset shmem_data.tex1_offset %zu\n", offsetof(struct shmem_data, tex1_offset));
			printf("offset shmem_data.tex2_offset %zu\n", offsetof(struct shmem_data, tex2_offset));
			printf("CAPTURE_TYPE_MEMORY %d CAPTURE_TYPE_TEXTURE %d\n", CAPTURE_TYPE_MEMORY, CAPTURE_TYPE_TEXTURE);
		} else if (!strcmp(c, "hook_set")) { /* field value */
			const char *k = arg(&sl, 1);
			uint64_t v = (uint64_t)strtoull(arg(&sl, 2), NULL, 0);
			if (!strcmp(k, "ver_major")) hi.hook_ver_major = (uint32_t)v;
			else if (!strcmp(k, "ver_minor")) hi.hook_ver_minor = (uint32_t)v;
			else if (!strcmp(k, "type")) hi.type = (enum capture_type)v;
			else if (!strcmp(k, "window")) hi.window = (uint32_t)v;
			else if (!strcmp(k, "format")) hi.format = (uint32_t)v;
			else if (!strcmp(k, "cx")) hi.cx = (uint32_t)v;
			else if (!strcmp(k, "cy")) hi.cy = (uint32_t)v;
			else if (!strcmp(k, "pitch")) hi.pitch = (uint32_t)v;
			else if (!strcmp(k, "map_id")) hi.map_id = (uint32_t)v;
			else if (!strcmp(k, "map_size")) hi.map_size = (uint32_t)v;
			else if (!strcmp(k, "flip")) hi.flip = v != 0;
			else if (!strcmp(k, "frame_interval")) hi.frame_interval = v;
			else if (!strcmp(k, "force_shmem")) hi.force_shmem = v != 0;
			else if (!strcmp(k, "capture_overlay")) hi.capture_overlay = v != 0;
			else if (!strcmp(k, "allow_srgb_alias")) hi.allow_srgb_alias = v != 0;
			else if (!strcmp(k, "d3d9_present")) hi.offsets.d3d9.present = (uint32_t)v;
			else if (!strcmp(k, "dxgi_present")) hi.offsets.dxgi.present = (uint32_t)v;
			else if (!strcmp(k, "dxgi_resize")) hi.offsets.dxgi.resize = (uint32_t)v;
			else if (!strcmp(k, "dxgi_present1")) hi.offsets.dxgi.present1 = (uint32_t)v;
			else if (!strcmp(k, "dxgi2_release")) hi.offsets.dxgi2.release = (uint32_t)v;
			else if (!strcmp(k, "d3d12_ecl")) hi.offsets.d3d12.execute_command_lists = (uint32_t)v;
			else if (!strcmp(k, "reserved0")) hi.reserved[0] = (uint32_t)v;
			else if (!strcmp(k, "reserved125")) hi.reserved[125] = (uint32_t)v;
			else printf("unknown hook field %s\n", k);
		} else if (!strcmp(c, "hook_hex")) { /* bytes [from] [count] of the struct */
			size_t from = (size_t)num(&sl, 1), n = sl.argc > 2 ? (size_t)num(&sl, 2) : sizeof(hi) - from;
			hex((const uint8_t *)&hi + from, n);
		} else if (!strcmp(c, "hook_names")) {
#define NM(x) do { char b[96]; size_t i = 0; for (; x[i]; i++) b[i] = (char)x[i]; b[i] = 0; printf("%s %s\n", #x, b); } while (0)
			NM(EVENT_CAPTURE_RESTART); NM(EVENT_CAPTURE_STOP); NM(EVENT_HOOK_READY); NM(EVENT_HOOK_EXIT);
				NM(EVENT_HOOK_INIT); NM(WINDOW_HOOK_KEEPALIVE); NM(MUTEX_TEXTURE1); NM(MUTEX_TEXTURE2);
				NM(SHMEM_HOOK_INFO); NM(SHMEM_TEXTURE);
			printf("PIPE_NAME %s\n", PIPE_NAME);
		} else if (!strcmp(c, "hook_mapping")) { /* pid */
			memset(last_map_name, 0, sizeof last_map_name);
			HANDLE h = create_hook_info((DWORD)num(&sl, 1));
			printf("create_hook_info handle=%d name=%s size=%zu\n", h != NULL, last_map_name,
			       h ? ((struct fmap *)h)->size : (size_t)0);
			if (h)
				CloseHandle(h);
		}
		/* ---------------------------------------------------------------- virtual camera queue (real C) */
		else if (!strcmp(c, "vq_create")) { /* slot cx cy interval */
			int s = (int)num(&sl, 1);
			VQ[s] = video_queue_create((uint32_t)num(&sl, 2), (uint32_t)num(&sl, 3), (uint64_t)num(&sl, 4));
			printf("vq_create[%d] %s\n", s, VQ[s] ? "ok" : "null");
		} else if (!strcmp(c, "vq_open")) {
			int s = (int)num(&sl, 1);
			VQ[s] = video_queue_open();
			printf("vq_open[%d] %s\n", s, VQ[s] ? "ok" : "null");
		} else if (!strcmp(c, "vq_close")) {
			int s = (int)num(&sl, 1);
			video_queue_close(VQ[s]);
			VQ[s] = NULL;
			printf("vq_close[%d]\n", s);
		} else if (!strcmp(c, "vq_state")) {
			printf("vq_state[%d] %d\n", (int)num(&sl, 1), (int)video_queue_state(VQ[num(&sl, 1)]));
		} else if (!strcmp(c, "vq_info")) {
			uint32_t cx, cy;
			uint64_t iv;
			video_queue_get_info(VQ[num(&sl, 1)], &cx, &cy, &iv);
			printf("vq_info %u %u %llu\n", cx, cy, (unsigned long long)iv);
		} else if (!strcmp(c, "vq_header")) { /* dumps queue_header fields of slot */
			struct queue_header *h = VQ[num(&sl, 1)]->header;
			printf("header write_idx=%u read_idx=%u state=%u offsets=%u,%u,%u type=%u cx=%u cy=%u interval=%llu\n",
			       h->write_idx, h->read_idx, h->state, h->offsets[0], h->offsets[1], h->offsets[2], h->type,
			       h->cx, h->cy, (unsigned long long)h->interval);
			printf("sizeof queue_header %zu\n", sizeof(struct queue_header));
			for (int i = 0; i < 8; i++)
				if (h->reserved[i])
					printf("reserved[%d] nonzero\n", i);
		} else if (!strcmp(c, "vq_write")) { /* slot ylinesize ybyte uvbyte timestamp */
			int s = (int)num(&sl, 1);
			struct queue_header *h = VQ[s]->header;
			uint32_t ls = (uint32_t)num(&sl, 2);
			size_t ysz = (size_t)ls * h->cy;
			uint8_t *y = malloc(ysz), *uv = malloc(ysz / 2);
			memset(y, (int)num(&sl, 3), ysz), memset(uv, (int)num(&sl, 4), ysz / 2);
			uint8_t *planes[2] = {y, uv};
			uint32_t lines[2] = {ls, ls};
			video_queue_write(VQ[s], planes, lines, (uint64_t)num(&sl, 5));
			free(y), free(uv);
			printf("vq_write write_idx=%u read_idx=%u state=%u\n", h->write_idx, h->read_idx, h->state);
		} else if (!strcmp(c, "vq_read")) { /* slot dstcx dstcy  (NV12 scale to dst) */
			int s = (int)num(&sl, 1);
			uint32_t scx, scy;
			uint64_t iv;
			video_queue_get_info(VQ[s], &scx, &scy, &iv);
			int dcx = (int)num(&sl, 2), dcy = (int)num(&sl, 3);
			nv12_scale_t sc;
			nv12_scale_init(&sc, TARGET_FORMAT_NV12, dcx, dcy, (int)scx, (int)scy);
			size_t sz = (size_t)dcx * dcy * 3 / 2;
			uint8_t *dst = malloc(sz);
			memset(dst, 0xEE, sz);
			uint64_t ts = 0xDEAD;
			bool ok = video_queue_read(VQ[s], &sc, dst, &ts);
			printf("vq_read[%d] %s ts=%llu", s, ok ? "true" : "false", (unsigned long long)ts);
			if (ok) {
				printf(" y0=%02x yLast=%02x uv0=%02x uvLast=%02x", dst[0], dst[(size_t)dcx * dcy - 1],
				       dst[(size_t)dcx * dcy], dst[sz - 1]);
			}
			printf("\n");
			free(dst);
		}
		/* ---------------------------------------------------------------- MODEL commands */
		else if (!strcmp(c, "m_wgc_supported")) {
			wgc_supported = num(&sl, 1) != 0;
		} else if (!strcmp(c, "m_wc_method")) { /* method(0..2) class */
			printf("wc_method %s class=%s -> %s\n", method_name((int)num(&sl, 1)), nz(arg(&sl, 2)) ? arg(&sl, 2) : "(null)",
		       method_name(m_wc_method((int)num(&sl, 1), wgc_supported, nz(arg(&sl, 2)))));
		} else if (!strcmp(c, "m_wc_update")) { /* parse S like update_settings */
			char *k, *t, *e;
			ms_build_window_strings(obs_data_get_string(&S, "window"), &k, &t, &e);
			int m = (int)obs_data_get_int(&S, "method");
			printf("method_selected %s method_chosen %s priority %d\n", method_name(m),
			       method_name(m_wc_method(m, wgc_supported, k)), (int)obs_data_get_int(&S, "priority"));
			printf("cursor %d capture_audio %d force_sdr %d compatibility %d client_area %d\n",
			       obs_data_get_bool(&S, "cursor"), obs_data_get_bool(&S, "capture_audio"),
			       obs_data_get_bool(&S, "force_sdr"), obs_data_get_bool(&S, "compatibility"),
			       obs_data_get_bool(&S, "client_area"));
			pstr("title", t), pstr("class", k), pstr("exe", e);
			bfree(k), bfree(t), bfree(e);
		} else if (!strcmp(c, "m_wc_visibility")) { /* chosen-method cursor_toggle_supported */
			int m = (int)num(&sl, 1);
			bool tog = num(&sl, 2) != 0;
			printf("visible cursor=%d compatibility=%d client_area=%d force_sdr=%d\n",
			       (m == 1) || (m == 2 && tog), m == 1, m == 2, m == 2);
		} else if (!strcmp(c, "m_dup_method")) { /* method wgc dxgi_index battery_flag(-1=call fails) adapters */
			int m = (int)num(&sl, 1);
			bool wgc = num(&sl, 2) != 0;
			int dxgi_index = (int)num(&sl, 3), battery = (int)num(&sl, 4), adapters = (int)num(&sl, 5);
			if (!wgc)
				m = 1;
			if (m == 0) {
				m = 1;
				if (dxgi_index == -1)
					m = 2;
				else if (battery >= 0 && battery < 128 && adapters >= 2)
					m = 2;
			}
			printf("dup_method %s\n", m == 0 ? "auto" : m == 1 ? "dxgi" : "wgc");
		} else if (!strcmp(c, "m_hook_rate")) {
			int r = (int)num(&sl, 1);
			float v = r == 0 ? 2.0f : r == 2 ? 0.5f : r == 3 ? 0.1f : 1.0f;
			printf("hook_rate %d -> %.9g\n", r, v);
		} else if (!strcmp(c, "m_convert_format")) { /* dxgi format number */
			int d = (int)num(&sl, 1);
			const char *n = d == 28 ? "RGBA" : d == 88 ? "BGRX" : d == 87 ? "BGRA" : d == 24 ? "R10G10B10A2" :
					d == 11 ? "RGBA16" : d == 10 ? "RGBA16F" : d == 2 ? "RGBA32F" : "UNKNOWN";
			printf("convert_format %d -> %s sixteen_bit=%d\n", d, n, d == 86 || d == 85);
		} else if (!strcmp(c, "m_gc_config")) { /* window string from store key "window" */
			char *k, *t, *e;
			ms_build_window_strings(obs_data_get_string(&S, "window"), &k, &t, &e);
			const char *ms;
			bool older = obs_data_has_user_value(&S, "capture_any_fullscreen") &&
				     !obs_data_has_user_value(&S, "capture_mode");
			if (older)
				ms = obs_data_get_bool(&S, "capture_any_fullscreen") ? "any_fullscreen" : "window";
			else
				ms = obs_data_get_string(&S, "capture_mode");
			int mode = !strcmp(ms, "window") ? 1 : !strcmp(ms, "hotkey") ? 2 : 0;
			printf("older_format %d mode %s priority %d\n", older, mode == 1 ? "window" : mode == 2 ? "hotkey" : "any",
			       (int)obs_data_get_int(&S, "priority"));
			printf("force_shmem %d cursor %d transparency %d premultiplied %d limit_fps %d overlays %d anticheat %d\n",
			       obs_data_get_bool(&S, "sli_compatibility"), obs_data_get_bool(&S, "capture_cursor"),
			       obs_data_get_bool(&S, "allow_transparency"), obs_data_get_bool(&S, "premultiplied_alpha"),
			       obs_data_get_bool(&S, "limit_framerate"), obs_data_get_bool(&S, "capture_overlays"),
			       obs_data_get_bool(&S, "anti_cheat_hook"));
			printf("hook_rate %d is_10a2_2100pq %d capture_audio %d\n", (int)obs_data_get_int(&S, "hook_rate"),
			       strcmp(obs_data_get_string(&S, "rgb10a2_space"), "2100pq") == 0,
			       obs_data_get_bool(&S, "capture_audio"));
			pstr("title", t), pstr("class", k), pstr("exe", e);
			/* property visibility (mode_callback) */
			bool cw = older ? !obs_data_get_bool(&S, "capture_any_fullscreen") :
					  strcmp(obs_data_get_string(&S, "capture_mode"), "window") == 0;
			printf("window_and_priority_visible %d\n", cw);
			bfree(k), bfree(t), bfree(e);
		} else if (!strcmp(c, "m_audio_changed")) { /* old_window old_prio new_window new_prio */
			const char *ow = arg(&sl, 1), *nw_ = arg(&sl, 3);
			int op = (int)num(&sl, 2), np = (int)num(&sl, 4);
			const char *o = !strcmp(ow, "-") ? "" : ow, *n = !strcmp(nw_, "-") ? "" : nw_;
			bool ch = (op != np && *n) || strcmp(o, n) != 0;
			printf("settings_changed %d\n", ch);
		} else if (!strcmp(c, "m_ks_to_layout")) { /* mask channels */
			printf("ks_to_layout mask=0x%x channels=%d -> %d\n", (unsigned)num(&sl, 1), (int)num(&sl, 2),
			       m_ks_to_layout((uint32_t)num(&sl, 1), (uint16_t)num(&sl, 2)));
		} else if (!strcmp(c, "m_layout_to_ks")) {
			printf("layout_to_ks %d -> 0x%x\n", (int)num(&sl, 1), m_layout_to_ks((int)num(&sl, 1)));
		} else if (!strcmp(c, "m_wasapi_new")) { /* type: input|device|process  (uses store) */
			memset(&WS, 0, sizeof(WS));
			WS.type = !strcmp(arg(&sl, 1), "input") ? 0 : !strcmp(arg(&sl, 1), "device") ? 1 : 2;
			wasapi_params(&WS);
			printf("wasapi_new device_id=%s timing=%d default=%d priority=%d class=%s title=%s exe=%s\n",
			       WS.device_id, WS.timing, WS.is_default, WS.priority, WS.cls, WS.title, WS.exe);
		} else if (!strcmp(c, "m_wasapi_update")) {
			struct wsrc np;
			memset(&np, 0, sizeof(np));
			np.type = WS.type;
			wasapi_params(&np);
			bool restart = WS.type == 2 ? (WS.priority != np.priority || strcmp(WS.cls, np.cls) ||
						       strcmp(WS.title, np.title) || strcmp(WS.exe, np.exe))
						    : strcmp(WS.device_id, np.device_id) != 0;
			bool notify = np.type == 1 && strcmp(WS.device_id, np.device_id) != 0;
			char keep[100];
			memcpy(keep, WS.default_id, sizeof keep);
			WS = np;
			memcpy(WS.default_id, keep, sizeof keep);
			printf("wasapi_update restart=%d capture_device_changed_signal=%d device_id=%s default=%d timing=%d\n", restart,
			       notify, WS.device_id, WS.is_default, WS.timing);
		} else if (!strcmp(c, "m_wasapi_default")) { /* flow(0 render,1 capture) role(0 console,1 multimedia,2 comm) id|- */
			int flow = (int)num(&sl, 1), role = (int)num(&sl, 2);
			const char *id = nz(arg(&sl, 3));
			bool input = WS.type == 0;
			bool restart = false;
			if (WS.is_default && flow == (input ? 1 : 0) && role == (input ? 2 : 0)) {
				if (id) {
					if (strcmp(WS.default_id, id) != 0) {
						snprintf(WS.default_id, sizeof WS.default_id, "%s", id);
						restart = true;
					}
				} else if (WS.default_id[0]) {
					WS.default_id[0] = 0;
					restart = true;
				}
			}
			printf("wasapi_default restart=%d tracked=%s\n", restart, WS.default_id);
		} else if (!strcmp(c, "m_dshow_res_parse")) {
			int cx = 0, cy = 0;
			const char *s = arg(&sl, 1);
			int n = sscanf(s, "%dx%d", &cx, &cy);
			bool valid = *s && n == 2;
			printf("res_parse valid=%d cx=%d cy=%d\n", valid, valid ? cx : 0, valid ? cy : 0);
		} else if (!strcmp(c, "m_dshow_caps_reset")) {
			ncaps = 0;
		} else if (!strcmp(c, "m_dshow_cap")) { /* fmt(any|N) minCX minCY maxCX maxCY minInterval maxInterval */
			struct cap *cp = &CAPS[ncaps++];
			cp->fmt = !strcmp(arg(&sl, 1), "any") ? FMT_ANY : (int)num(&sl, 1);
			cp->minCX = (int)num(&sl, 2), cp->minCY = (int)num(&sl, 3), cp->maxCX = (int)num(&sl, 4),
			cp->maxCY = (int)num(&sl, 5), cp->minI = num(&sl, 6), cp->maxI = num(&sl, 7);
		} else if (!strcmp(c, "m_dshow_select")) { /* cx cy interval fmt obs_interval */
			int cx = (int)num(&sl, 1), cy = (int)num(&sl, 2);
			long long interval = num(&sl, 3);
			int fmt = !strcmp(arg(&sl, 4), "any") ? FMT_ANY : (int)num(&sl, 4);
			if (interval == FPS_MATCHING)
				interval = num(&sl, 5);
			long long best = LLONG_MAX;
			bool fmt_match = false, caps_match = false;
			for (int i = 0; i < ncaps; i++) {
				struct cap *cp = &CAPS[i];
				bool ok = between(cp->minCX, cx, cp->maxCX) && between(cp->minCY, cy, cp->maxCY);
				if (ok) {
					bool fm = fmt == FMT_ANY || cp->fmt == FMT_ANY || fmt == cp->fmt;
					fmt_match = fmt_match || fm;
					ok = fm;
					if (ok) {
						long long cur = fr_interval(cp, interval);
						if (llabs(interval - best) > llabs(interval - cur))
							best = cur;
						ok = fr_available(cp, interval);
					}
				}
				if (ok)
					caps_match = true;
			}
			if (!caps_match && !fmt_match)
				printf("select fail (video format match failed)\n");
			else
				printf("select ok interval=%lld caps_match=%d format_match=%d\n", best, caps_match,
				       fmt_match);
		} else if (!strcmp(c, "m_dshow_res_reset")) {
			nres = 0;
		} else if (!strcmp(c, "m_dshow_res_addcap")) { /* minCX minCY maxCX maxCY */
			insert_resolution((int)num(&sl, 1), (int)num(&sl, 2));
			insert_resolution((int)num(&sl, 3), (int)num(&sl, 4));
		} else if (!strcmp(c, "m_dshow_res_dump")) {
			for (int i = 0; i < nres; i++)
				printf("%dx%d%s", RES[i].cx, RES[i].cy, i + 1 < nres ? " " : "\n");
			if (!nres)
				printf("(empty)\n");
		} else if (!strcmp(c, "m_dshow_buffering")) { /* bufType name fmt_after_mjpeg */
			int bt = (int)num(&sl, 1);
			const char *name = arg(&sl, 2);
			bool after = num(&sl, 3) != 0;
			char lo[300];
			size_t i = 0;
			for (; name[i] && i < 299; i++)
				lo[i] = (char)tolower((unsigned char)name[i]);
			lo[i] = 0;
			bool delayed = after || strstr(lo, "elgato game capture hd") || strstr(lo, "stream engine");
			bool use = bt == 0 ? delayed : bt == 1;
			bool decoupled = strstr(lo, "gv-usb2") != NULL;
			printf("buffering use=%d async_unbuffered=%d decoupled=%d\n", use, !use, decoupled);
		} else if (!strcmp(c, "m_dshow_fps")) { /* fps_num fps_den  -> GetOBSFPS */
			uint32_t n = (uint32_t)num(&sl, 1), d = (uint32_t)num(&sl, 2);
			printf("obs_fps_interval %lld\n", (long long)d * 10000000LL / n);
		} else if (!strcmp(c, "m_denc_update")) { /* w h fps_num fps_den keyint_sec */
			int w = (int)num(&sl, 1), h = (int)num(&sl, 2);
			uint32_t fn = (uint32_t)num(&sl, 3), fd = (uint32_t)num(&sl, 4);
			int ks = (int)num(&sl, 5);
			double aspect = (double)w / (double)h;
			if (ks == 0)
				ks = 2;
			if (fabs(aspect - 1024.0 / 768.0) < fabs(aspect - 1280.0 / 720.0))
				w = 1024, h = 768;
			else
				w = 1280, h = 720;
			int keyint = ks * fn / fd; /* unsigned arithmetic, as the reference */
			long long interval = fd * 10000000 / fn; /* uint32 multiplication wraps: DEFECT D-1 */
			printf("denc_update %dx%d keyint=%d frameInterval=%lld\n", w, h, keyint, interval);
		} else if (!strcmp(c, "m_denc_videoinfo")) { /* fmt(i420|other) w h */
			bool i420 = !strcmp(arg(&sl, 1), "i420");
			int w = (int)num(&sl, 2), h = (int)num(&sl, 3);
			bool valid = (w == 1280 && h == 720) || (w == 1024 && h == 768);
			if (i420 && valid) {
				printf("videoinfo unchanged i420 %dx%d\n", w, h);
			} else {
				double aspect = (double)w / (double)h;
				if (fabs(aspect - 1024.0 / 768.0) < fabs(aspect - 1280.0 / 720.0))
					w = 1024, h = 768;
				else
					w = 1280, h = 720;
				printf("videoinfo forced i420 %dx%d range=default colorspace=default\n", w, h);
			}
		} else if (!strcmp(c, "compat_json")) { /* writes the compatibility file and drops the cached list */
			const char *dir = getenv("CONFORMANCE_TMPDIR");
			snprintf(compat_path, sizeof compat_path, "%s/wincapture-compat.json", dir ? dir : ".");
			if (compat_entries) {
				compat_json_free();
				compat_entries = NULL; /* the reference leaves a dangling pointer (D-2) */
			}
			if (!strcmp(arg(&sl, 1), "-")) {
				remove(compat_path);
			} else {
				FILE *fp = fopen(compat_path, "wb");
				fputs(arg(&sl, 1), fp);
				fclose(fp);
			}
		} else if (!strcmp(c, "compat_tr")) { /* translation_key text */
			snprintf(compat_tr[ncompat_tr].key, 100, "%s", arg(&sl, 1));
			snprintf(compat_tr[ncompat_tr].text, 300, "%s", arg(&sl, 2));
			ncompat_tr++;
		} else if (!strcmp(c, "compat_check")) { /* type(game|bitblt|wgc) title class exe */
			enum source_type t = !strcmp(arg(&sl, 1), "game") ? GAME_CAPTURE :
					     !strcmp(arg(&sl, 1), "bitblt") ? WINDOW_CAPTURE_BITBLT : WINDOW_CAPTURE_WGC;
			struct compat_result *r = check_compatibility(nz(arg(&sl, 2)), nz(arg(&sl, 3)), nz(arg(&sl, 4)), t);
			if (r) {
				printf("compat severity=%d\n", (int)r->severity);
				pstr("  message", r->message);
				compat_result_free(r);
			} else {
				printf("compat none\n");
			}
		} else if (!strcmp(c, "m_shmem_layout")) { /* cy pitch base_mod32 */
			uint32_t tex_size = (uint32_t)num(&sl, 1) * (uint32_t)num(&sl, 2);
			uint32_t base = (uint32_t)num(&sl, 3);
			uint32_t aligned_header = (uint32_t)((sizeof(struct shmem_data) + 31) & ~31u);
			uint32_t aligned_tex = (tex_size + 31) & ~31u;
			uint32_t total = aligned_header + aligned_tex * 2 + 32;
			uint32_t pos = ((base + aligned_header) & ~31u) - base;
			if (pos < sizeof(struct shmem_data))
				pos += 32;
			printf("shmem total=%u tex1_offset=%u tex2_offset=%u\n", total, pos, pos + aligned_tex);
		} else if (!strcmp(c, "m_frame_interval")) { /* fps_num fps_den limit(0|1) */
			uint64_t iv = util_mul_div64((uint64_t)num(&sl, 2), 1000000000ULL, (uint64_t)num(&sl, 1));
			if (!num(&sl, 3))
				iv /= 2;
			printf("frame_interval %llu\n", (unsigned long long)iv);
		} else if (!strcmp(c, "m_hook_ver_ok")) { /* hook_ver_major */
			printf("hook_ver_ok %d\n", (uint32_t)num(&sl, 1) <= HOOK_VER_MAJOR);
		} else {
			fprintf(stderr, "wincapture: unknown command '%s' at line %d\n", c, lineno);
			return 2;
		}
	}
	return 0;
}
