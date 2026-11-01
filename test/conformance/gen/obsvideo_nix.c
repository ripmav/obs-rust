/*
 * Conformance runner for area obsvideo (spec S-058), part 2: the Linux platform glue.
 *
 * The REAL libobs/obs-nix.c, obs-nix-platform.c and obs-nix-x11.c are #included here. Faked: the X11
 * display connection (Xlib / xcb: setup, keyboard mapping, keymap query, XInput raw events, XLookupString)
 * and the hotkey translation lookup of obs-hotkey.c. libobs/obs-hotkey-name-map.c (obs_key_to_name) is
 * linked for real. obs-nix-wayland.c is NOT covered (needs a Wayland compositor and xkbcommon).
 * Test tooling, not part of libobs.
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libgen.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "obsvideo_api.h"

#include "obs-internal.h"
/* The prefix is a relative path so that the install-prefix lookup branch can be exercised inside the
 * runner's working directory (the generated obsconfig.h of the test tree has an empty prefix). */
#undef OBS_INSTALL_PREFIX
#define OBS_INSTALL_PREFIX "prefix"

#include "obs-nix.c"
#include "obs-nix-platform.c"
#include "obs-nix-x11.c"

/* ---- working directories ---- */
static char g_work[512];

void nix_reexec(char **argv)
{
	if (getenv("OBSVIDEO_WORK")) {
		snprintf(g_work, sizeof g_work, "%s", getenv("OBSVIDEO_WORK"));
		char cwd[600];
		snprintf(cwd, sizeof cwd, "%s/cwd", g_work);
		if (chdir(cwd) != 0) {
			perror("chdir");
			exit(2);
		}
		return;
	}
	const char *tmp = getenv("CONFORMANCE_TMPDIR");
	if (!tmp)
		tmp = ".";
	char work[512], path[640];
	snprintf(work, sizeof work, "%s/obsvideo-work-%d", tmp, (int)getpid());
	mkdir(work, 0755);
	snprintf(path, sizeof path, "%s/bin", work);
	mkdir(path, 0755);
	snprintf(path, sizeof path, "%s/cwd", work);
	mkdir(path, 0755);
	snprintf(path, sizeof path, "%s/bin/runner", work);
	int in = open("/proc/self/exe", O_RDONLY);
	int out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (in < 0 || out < 0) {
		perror("copy exe");
		exit(2);
	}
	char buf[65536];
	ssize_t n;
	while ((n = read(in, buf, sizeof buf)) > 0)
		if (write(out, buf, (size_t)n) != n)
			exit(2);
	close(in);
	close(out);
	setenv("OBSVIDEO_WORK", work, 1);
	if (argv[1] && argv[1][0] != '/') { /* the re-executed process runs in another directory */
		static char abs[1024];
		if (realpath(argv[1], abs))
			argv[1] = abs;
	}
	execv(path, argv);
	perror("execv");
	exit(2);
}

static void mkpath(char *path)
{
	for (char *p = path + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			mkdir(path, 0755);
			*p = '/';
		}
	}
}

/* ---- fake X server ---- */
static __typeof__(*(_XPrivDisplay)0) g_xpriv; /* the layout the Xlib macros read */
#define g_dpy g_xpriv
static bool g_server_up = true;
static int g_default_screen;
static xcb_setup_t g_setup;
static xcb_screen_t g_screen = {.root = 0x1234};
static xcb_keysym_t g_map[256 * 8];
static int g_syms_per_code = 4;
static bool g_kb_fail;     /* keyboard mapping reply fails */
static bool g_keymap_fail; /* query_keymap fails */
static uint8_t g_keys_down[32];
static char g_lookup[256][8]; /* XLookupString text per keycode ("" = none) */
static int g_open_calls, g_close_calls, g_flush_calls, g_select_events_calls;
static int g_free_count;

struct fevent {
	uint16_t type;
	uint32_t detail;
};
static struct fevent g_events[64];
static int g_nevents, g_event_pos;

Display *XOpenDisplay(const char *name)
{
	(void)name;
	g_open_calls++;
	return g_server_up ? (Display *)&g_dpy : NULL;
}
int XCloseDisplay(Display *d)
{
	(void)d;
	g_close_calls++;
	return 0;
}
xcb_connection_t *XGetXCBConnection(Display *d)
{
	(void)d;
	return (xcb_connection_t *)&g_setup; /* any non-NULL pointer */
}
char *XServerVendor(Display *d)
{
	(void)d;
	return g_dpy.vendor ? g_dpy.vendor : (char *)"";
}
int XVendorRelease(Display *d)
{
	(void)d;
	return g_dpy.release;
}
int XProtocolVersion(Display *d)
{
	(void)d;
	return g_dpy.proto_major_version;
}
int XProtocolRevision(Display *d)
{
	(void)d;
	return g_dpy.proto_minor_version;
}
int XDefaultScreen(Display *d)
{
	(void)d;
	return g_default_screen;
}
const xcb_setup_t *xcb_get_setup(xcb_connection_t *c)
{
	(void)c;
	return &g_setup;
}
xcb_screen_iterator_t xcb_setup_roots_iterator(const xcb_setup_t *s)
{
	(void)s;
	xcb_screen_iterator_t it = {.data = &g_screen, .rem = 1, .index = 0};
	return it;
}
void xcb_screen_next(xcb_screen_iterator_t *i)
{
	i->rem--;
	i->data++;
	i->index++;
}
xcb_get_keyboard_mapping_cookie_t xcb_get_keyboard_mapping(xcb_connection_t *c, xcb_keycode_t first, uint8_t count)
{
	(void)c;
	(void)first;
	(void)count;
	xcb_get_keyboard_mapping_cookie_t k = {0};
	return k;
}
xcb_get_keyboard_mapping_reply_t *xcb_get_keyboard_mapping_reply(xcb_connection_t *c,
								 xcb_get_keyboard_mapping_cookie_t k,
								 xcb_generic_error_t **e)
{
	(void)c;
	(void)k;
	if (g_kb_fail) {
		*e = calloc(1, sizeof(**e));
		return NULL;
	}
	xcb_get_keyboard_mapping_reply_t *r = calloc(1, sizeof(*r));
	r->keysyms_per_keycode = (uint8_t)g_syms_per_code;
	return r;
}
xcb_keysym_t *xcb_get_keyboard_mapping_keysyms(const xcb_get_keyboard_mapping_reply_t *r)
{
	(void)r;
	return g_map;
}
xcb_query_keymap_cookie_t xcb_query_keymap(xcb_connection_t *c)
{
	(void)c;
	xcb_query_keymap_cookie_t k = {0};
	return k;
}
xcb_query_keymap_reply_t *xcb_query_keymap_reply(xcb_connection_t *c, xcb_query_keymap_cookie_t k,
						 xcb_generic_error_t **e)
{
	(void)c;
	(void)k;
	if (g_keymap_fail) {
		*e = calloc(1, sizeof(**e));
		return calloc(1, sizeof(xcb_query_keymap_reply_t));
	}
	xcb_query_keymap_reply_t *r = calloc(1, sizeof(*r));
	memcpy(r->keys, g_keys_down, 32);
	return r;
}
int xcb_flush(xcb_connection_t *c)
{
	(void)c;
	g_flush_calls++;
	return 1;
}
xcb_void_cookie_t xcb_input_xi_select_events(xcb_connection_t *c, xcb_window_t w, uint16_t n,
					     const xcb_input_event_mask_t *m)
{
	(void)c;
	g_select_events_calls++;
	printf("xi_select_events window=0x%x n=%u deviceid=%u mask_len=%u mask=0x%x\n", (unsigned)w, (unsigned)n,
	       (unsigned)m->deviceid, (unsigned)m->mask_len, (unsigned)((const uint32_t *)(m + 1))[0]);
	xcb_void_cookie_t k = {0};
	return k;
}
xcb_generic_event_t *xcb_poll_for_event(xcb_connection_t *c)
{
	(void)c;
	if (g_event_pos >= g_nevents)
		return NULL;
	struct fevent *fe = &g_events[g_event_pos++];
	xcb_input_raw_button_press_event_t *e = calloc(1, sizeof(*e));
	e->response_type = fe->type == 0xffff ? 2 : XCB_GE_GENERIC; /* 0xffff: not a generic event */
	e->event_type = fe->type == 0xffff ? 0 : fe->type;
	e->detail = fe->detail;
	return (xcb_generic_event_t *)e;
}
int XLookupString(XKeyEvent *ev, char *buf, int n, KeySym *k, XComposeStatus *s)
{
	(void)k;
	(void)s;
	const char *t = g_lookup[ev->keycode & 255];
	int len = (int)strlen(t);
	if (len > n)
		len = n;
	memcpy(buf, t, (size_t)len);
	return len;
}

/* ---- fake obs-hotkey.c bits ---- */
const char *obs_get_hotkey_translation(obs_key_t key, const char *def)
{
	if (key >= 0 && key < OBS_KEY_LAST_VALUE && obs->hotkeys.translations[key])
		return obs->hotkeys.translations[key];
	return def;
}

/* ---- keysym names usable in scripts ---- */
static const struct {
	const char *n;
	unsigned long v;
} g_ksyms[] = {
#define K(x) {#x, x}
	K(XK_Return), K(XK_Escape), K(XK_Tab), K(XK_BackSpace), K(XK_Insert), K(XK_Delete), K(XK_Home),
	K(XK_End), K(XK_Left), K(XK_Up), K(XK_Right), K(XK_Down), K(XK_Prior), K(XK_Next), K(XK_Shift_L),
	K(XK_Shift_R), K(XK_Control_L), K(XK_Control_R), K(XK_Alt_L), K(XK_Alt_R), K(XK_Super_L),
	K(XK_Super_R), K(XK_Menu), K(XK_Num_Lock), K(XK_a), K(XK_A), K(XK_b), K(XK_1), K(XK_exclam), K(XK_space),
	K(XK_F1), K(XK_F2), K(XK_KP_0), K(XK_KP_1), K(XK_KP_Enter), K(XK_KP_Add), K(XK_Caps_Lock),
	K(XK_Scroll_Lock), K(XK_Pause), K(XK_Print), K(XF86XK_AudioMute), K(XF86XK_AudioRaiseVolume),
	K(XF86XK_AudioLowerVolume), K(XF86XK_AudioPlay), K(XF86XK_AudioStop), K(XF86XK_AudioNext),
	K(XF86XK_AudioPrev), K(XK_Help), K(XK_Break), K(XK_Sys_Req), K(XK_Clear), K(XK_Select), K(XK_Execute),
	K(XK_Cancel), K(XK_Mode_switch), K(XK_Hyper_L), K(XK_Meta_L), K(XK_section), K(XK_bracketleft),
	K(XK_odiaeresis), K(XK_ssharp), K(XK_dead_grave), K(XK_KP_Decimal), K(XK_KP_Multiply),
	K(XK_KP_Subtract), K(XK_KP_Divide), K(XK_F35), K(XK_comma), K(XK_period),
#undef K
};

static unsigned long parse_sym(const char *s)
{
	for (size_t i = 0; i < sizeof g_ksyms / sizeof g_ksyms[0]; i++)
		if (!strcmp(g_ksyms[i].n, s))
			return g_ksyms[i].v;
	return strtoul(s, NULL, 0);
}

static obs_key_t parse_key(const char *s)
{
	if (!strncmp(s, "#", 1))
		return (obs_key_t)atoi(s + 1);
	return obs_key_from_name(s);
}

static void print_key(obs_key_t k)
{
	const char *n = obs_key_to_name(k);
	printf("%s", n ? n : "(null)");
}

/* ---- captured system info log ---- */
static char g_cap[64][256];
static int g_ncap;
static void cap_handler(int lvl, const char *fmt, va_list ap, void *p)
{
	(void)lvl;
	(void)p;
	if (g_ncap < 64) {
		vsnprintf(g_cap[g_ncap], 256, fmt, ap);
		g_ncap++;
	}
}

static bool starts(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }

bool nix_command(struct script_line *sl)
{
	const char *c = sl->argv[0];
	if (strncmp(c, "nix_", 4) && strncmp(c, "x11_", 4) && strncmp(c, "hk_", 3))
		return false;

	if (!strcmp(c, "nix_platform_get")) {
		printf("platform=%d display=%s\n", (int)obs_get_nix_platform(),
		       obs_get_nix_platform_display() ? "set" : "null");
	} else if (!strcmp(c, "nix_platform_set")) {
		script_need(sl, 2);
		obs_set_nix_platform((enum obs_nix_platform_type)atoi(sl->argv[1]));
	} else if (!strcmp(c, "nix_display_set")) { /* nix_display_set fake|null */
		script_need(sl, 2);
		obs_set_nix_platform_display(!strcmp(sl->argv[1], "fake") ? (void *)&g_dpy : NULL);
	} else if (!strcmp(c, "nix_module_ext")) {
		printf("module_ext=%s\n", get_module_extension());
	} else if (!strcmp(c, "nix_file")) { /* nix_file cwd|exe|prefix RELNAME : create a data file */
		script_need(sl, 3);
		char path[700];
		const char *w = g_work;
		if (!strcmp(sl->argv[1], "cwd"))
			snprintf(path, sizeof path, "%s/cwd/data/libobs/libobs/%s", w, sl->argv[2]);
		else if (!strcmp(sl->argv[1], "exe"))
			snprintf(path, sizeof path, "%s/data/libobs/libobs/%s", w, sl->argv[2]);
		else
			snprintf(path, sizeof path, "%s/cwd/prefix/data/libobs/libobs/%s", w, sl->argv[2]);
		mkpath(path);
		int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0)
			close(fd);
	} else if (!strcmp(c, "nix_unfile")) {
		script_need(sl, 3);
		char path[700];
		const char *w = g_work;
		if (!strcmp(sl->argv[1], "cwd"))
			snprintf(path, sizeof path, "%s/cwd/data/libobs/libobs/%s", w, sl->argv[2]);
		else if (!strcmp(sl->argv[1], "exe"))
			snprintf(path, sizeof path, "%s/data/libobs/libobs/%s", w, sl->argv[2]);
		else
			snprintf(path, sizeof path, "%s/cwd/prefix/data/libobs/libobs/%s", w, sl->argv[2]);
		remove(path);
	} else if (!strcmp(c, "nix_find")) { /* nix_find NAME: print the located path with the private root masked */
		script_need(sl, 2);
		char *r = find_libobs_data_file(sl->argv[1]);
		if (!r) {
			printf("find %s -> (null)\n", sl->argv[1]);
		} else {
			const char *p = r;
			size_t wl = strlen(g_work);
			if (!strncmp(p, g_work, wl)) {
				printf("find %s -> <work>%s\n", sl->argv[1], p + wl);
			} else {
				printf("find %s -> %s\n", sl->argv[1], p);
			}
			bfree(r);
		}
	} else if (!strcmp(c, "nix_env")) { /* nix_env NAME VALUE|- */
		script_need(sl, 3);
		if (!strcmp(sl->argv[2], "-"))
			unsetenv(sl->argv[1]);
		else
			setenv(sl->argv[1], sl->argv[2], 1);
	} else if (!strcmp(c, "nix_system_info")) {
		/* Only the deterministic part of log_system_info() is printed: the machine specific values
		 * (CPU, memory, kernel, distribution, flatpak) are reduced to the label before the colon. */
		g_ncap = 0;
		base_set_log_handler(cap_handler, NULL);
		log_system_info();
		base_set_log_handler(NULL, NULL);
		bool distro_done = false;
		for (int i = 0; i < g_ncap; i++) {
			char *l = g_cap[i];
			if (starts(l, "CPU Name:") || starts(l, "CPU Speed:"))
				continue; /* one pair per physical CPU, machine specific: not part of the vectors */
			if (starts(l, "Window System:") || starts(l, "Unable to open X display") ||
			    starts(l, "Desktop Environment:") || starts(l, "Session Type:")) {
				printf("sysinfo %s\n", l);
			} else if (starts(l, "Flatpak") || starts(l, "App Extensions") || starts(l, "Runtime Extensions") ||
				   starts(l, " - ") || starts(l, "Unable to open .flatpak-info") || starts(l, "Distribution:")) {
				if (!distro_done)
					printf("sysinfo <distribution-or-flatpak>\n");
				distro_done = true;
			} else {
				char *colon = strchr(l, ':');
				if (colon)
					*colon = 0;
				printf("sysinfo %s:\n", l);
			}
		}
	} else if (!strcmp(c, "x11_server")) { /* x11_server up|down */
		script_need(sl, 2);
		g_server_up = !strcmp(sl->argv[1], "up");
	} else if (!strcmp(c, "x11_vendor")) { /* x11_vendor VENDOR RELEASE PMAJOR PMINOR */
		script_need(sl, 5);
		static char vendor[128];
		snprintf(vendor, sizeof vendor, "%s", sl->argv[1]);
		g_dpy.vendor = vendor;
		g_dpy.release = atoi(sl->argv[2]);
		g_dpy.proto_major_version = atoi(sl->argv[3]);
		g_dpy.proto_minor_version = atoi(sl->argv[4]);
	} else if (!strcmp(c, "x11_screen")) {
		script_need(sl, 2);
		g_default_screen = atoi(sl->argv[1]);
	} else if (!strcmp(c, "x11_setup")) { /* x11_setup MINKC MAXKC SYMS_PER_CODE */
		script_need(sl, 4);
		g_setup.min_keycode = (xcb_keycode_t)atoi(sl->argv[1]);
		g_setup.max_keycode = (xcb_keycode_t)atoi(sl->argv[2]);
		g_syms_per_code = atoi(sl->argv[3]);
		memset(g_map, 0, sizeof g_map);
	} else if (!strcmp(c, "x11_map")) { /* x11_map KEYCODE SYM... : keysyms of a keycode (by name or number) */
		script_need(sl, 3);
		int code = atoi(sl->argv[1]) - g_setup.min_keycode;
		for (int i = 2; i < sl->argc && i - 2 < g_syms_per_code; i++)
			g_map[code * g_syms_per_code + (i - 2)] = (xcb_keysym_t)parse_sym(sl->argv[i]);
	} else if (!strcmp(c, "x11_kb_fail")) {
		script_need(sl, 2);
		g_kb_fail = atoi(sl->argv[1]) != 0;
	} else if (!strcmp(c, "x11_keymap_fail")) {
		script_need(sl, 2);
		g_keymap_fail = atoi(sl->argv[1]) != 0;
	} else if (!strcmp(c, "x11_keys_down")) { /* x11_keys_down CODE... ('-' alone clears) */
		memset(g_keys_down, 0, sizeof g_keys_down);
		for (int i = 1; i < sl->argc; i++)
			if (strcmp(sl->argv[i], "-")) {
				int code = atoi(sl->argv[i]);
				g_keys_down[code / 8] |= (uint8_t)(1 << (code % 8));
			}
	} else if (!strcmp(c, "x11_lookup")) { /* x11_lookup KEYCODE TEXT|- : XLookupString result */
		script_need(sl, 3);
		snprintf(g_lookup[atoi(sl->argv[1]) & 255], 8, "%s", strcmp(sl->argv[2], "-") ? sl->argv[2] : "");
	} else if (!strcmp(c, "x11_event")) { /* x11_event press|release|other|nongeneric DETAIL */
		script_need(sl, 3);
		struct fevent *fe = &g_events[g_nevents++];
		fe->detail = (uint32_t)atoi(sl->argv[2]);
		fe->type = !strcmp(sl->argv[1], "press")     ? XCB_INPUT_RAW_BUTTON_PRESS
			   : !strcmp(sl->argv[1], "release") ? XCB_INPUT_RAW_BUTTON_RELEASE
			   : !strcmp(sl->argv[1], "other")   ? 99
							     : 0xffff;
	} else if (!strcmp(c, "x11_stats")) {
		printf("x11 opens=%d closes=%d flushes=%d select_events=%d pending_events=%d\n", g_open_calls, g_close_calls,
		       g_flush_calls, g_select_events_calls, g_nevents - g_event_pos);
	} else if (!strcmp(c, "hk_init")) {
		memset(&obs->hotkeys, 0, sizeof obs->hotkeys);
		hotkeys_vtable = NULL;
		bool r = obs_hotkeys_platform_init(&obs->hotkeys);
		obs_hotkeys_platform_t *p = obs->hotkeys.platform_context;
		printf("hotkeys_platform_init -> %d context=%s", (int)r, p ? "set" : "null");
		if (p)
			printf(" min_keycode=%d super_l=%d super_r=%d num_keysyms=%d syms_per_code=%d", (int)p->min_keycode,
			       (int)p->super_l_code, (int)p->super_r_code, p->num_keysyms, p->syms_per_code);
		printf("\n");
	} else if (!strcmp(c, "hk_free")) {
		obs_hotkeys_platform_free(&obs->hotkeys);
		printf("hotkeys_platform_free context=%s vtable=%s\n", obs->hotkeys.platform_context ? "set" : "null",
		       hotkeys_vtable ? "set" : "null");
	} else if (!strcmp(c, "hk_keycodes")) { /* hk_keycodes KEY: keycodes learned for an obs key */
		script_need(sl, 2);
		obs_key_t k = parse_key(sl->argv[1]);
		struct keycode_list *l = &obs->hotkeys.platform_context->keycodes[k];
		printf("keycodes %s:", sl->argv[1]);
		for (size_t i = 0; i < l->list.num; i++)
			printf(" %d", (int)l->list.array[i]);
		printf("\n");
	} else if (!strcmp(c, "hk_base")) { /* hk_base KEY: base keysym of an obs key */
		script_need(sl, 2);
		obs_key_t k = parse_key(sl->argv[1]);
		printf("base_keysym %s = 0x%x\n", sl->argv[1], (unsigned)obs->hotkeys.platform_context->base_keysyms[k]);
	} else if (!strcmp(c, "hk_pressed")) {
		script_need(sl, 2);
		printf("is_pressed %s = %d\n", sl->argv[1], (int)obs_hotkeys_platform_is_pressed(obs->hotkeys.platform_context, parse_key(sl->argv[1])));
	} else if (!strcmp(c, "hk_translation")) { /* hk_translation KEY TEXT|- */
		script_need(sl, 3);
		obs_key_t k = parse_key(sl->argv[1]);
		static char tr[OBS_KEY_LAST_VALUE][32];
		if (!strcmp(sl->argv[2], "-")) {
			obs->hotkeys.translations[k] = NULL;
		} else {
			snprintf(tr[k], 32, "%s", sl->argv[2]);
			obs->hotkeys.translations[k] = tr[k];
		}
	} else if (!strcmp(c, "hk_to_str")) { /* hk_to_str KEY [prefill] */
		script_need(sl, 2);
		struct dstr d = {0};
		if (sl->argc > 2)
			dstr_copy(&d, sl->argv[2]);
		obs_key_t k = parse_key(sl->argv[1]);
		obs_key_to_str(k, &d);
		printf("key_to_str %s = ", sl->argv[1]);
		script_print_quoted(d.array ? d.array : "");
		printf("\n");
		dstr_free(&d);
	} else if (!strcmp(c, "hk_from_vk")) {
		script_need(sl, 2);
		obs_key_t k = obs_key_from_virtual_key((int)parse_sym(sl->argv[1]));
		printf("key_from_virtual_key %s = ", sl->argv[1]);
		print_key(k);
		printf("\n");
	} else if (!strcmp(c, "hk_to_vk")) {
		script_need(sl, 2);
		printf("key_to_virtual_key %s = 0x%x\n", sl->argv[1], (unsigned)obs_key_to_virtual_key(parse_key(sl->argv[1])));
	} else if (!strcmp(c, "hk_combo")) { /* hk_combo MODS KEY */
		script_need(sl, 3);
		obs_key_combination_t comb = {(uint32_t)strtoul(sl->argv[1], NULL, 0), parse_key(sl->argv[2])};
		struct dstr d = {0};
		obs_key_combination_to_str(comb, &d);
		printf("combination_to_str 0x%x %s = ", comb.modifiers, sl->argv[2]);
		script_print_quoted(d.array ? d.array : "");
		printf("\n");
		dstr_free(&d);
	} else {
		fprintf(stderr, "script error: unknown nix command %s\n", c);
		exit(2);
	}
	return true;
}
