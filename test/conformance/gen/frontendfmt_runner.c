/*
 * Conformance runner for the non-GUI logic of the frontend (spec S-048).
 *
 * IMPORTANT: the frontend is C++/Qt and cannot be linked without Qt. The pure functions below are
 * TRANSCRIBED into C from frontend/obs-main.cpp, frontend/OBSApp.cpp and frontend/widgets/OBSBasic.cpp
 * (same control flow, same constants). The configuration store is the REAL libobs config-file.c.
 * "Virtual file system" commands replace os_file_exists / os_opendir. Everything else (Qt, dialogs,
 * threads, real I/O) is out of scope for this runner.
 */
#include <ctype.h>
#include <inttypes.h>
#include "util/config-file.h"
#include "harness.h"

/* ---------------- virtual file system ---------------- */
#define MAXV 64
static char *vfiles[MAXV];
static int nvfiles;
static char *vdirs[MAXV];
static int nvdirs;

static bool vexists(const char *p)
{
	for (int i = 0; i < nvfiles; i++)
		if (!strcmp(vfiles[i], p))
			return true;
	return false;
}
static bool vdir(const char *p)
{
	size_t n = strlen(p);
	while (n > 1 && (p[n - 1] == '/' || p[n - 1] == '\\'))
		n--; /* opendir accepts trailing separators */
	for (int i = 0; i < nvdirs; i++)
		if (strlen(vdirs[i]) == n && !strncmp(vdirs[i], p, n))
			return true;
	return false;
}

/* ---------------- path / name helpers (OBSApp.cpp) ---------------- */

/* GetFileSafeName for ASCII input (see spec Q on non-ASCII / locale). */
static void file_safe_name(const char *in, char *out)
{
	char tmp[1024];
	size_t n = 0;
	for (size_t i = 0; in[i] && n < sizeof tmp - 1; i++)
		tmp[n++] = in[i];
	/* the reference iterates from the back erasing; the result equals a forward filter */
	size_t o = 0;
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)tmp[i];
		if (c == ' ' || (c >= 9 && c <= 13))
			out[o++] = '_';
		else if (c == '_' || isalnum(c))
			out[o++] = (char)c;
	}
	out[o] = 0;
	if (o == 0)
		strcpy(out, "characters_only");
}

static void closest_unused(char *path, const char *ext)
{
	size_t len = strlen(path);
	if (ext) {
		strcat(path, ".");
		strcat(path, ext);
	}
	if (!vexists(path))
		return;
	int index = 1;
	do {
		path[len] = 0;
		sprintf(path + len, "%d", ++index);
		if (ext) {
			strcat(path, ".");
			strcat(path, ext);
		}
	} while (vexists(path));
}

static void remove_reserved(char *s)
{
	for (; *s; s++) {
		switch (*s) {
		case '\\':
			*s = '/';
			break;
		case '*':
		case '?':
		case '"':
		case '|':
		case ':':
		case '>':
		case '<':
			*s = '_';
			break;
		}
	}
}

static void format_string(const char *format, const char *prefix, const char *suffix, char *out)
{
	char f[1024];
	snprintf(f, sizeof f, "%s", format);
	if (prefix && *prefix) {
		char p[256];
		snprintf(p, sizeof p, "%s", prefix);
		if (p[strlen(p) - 1] != ' ')
			strcat(p, " ");
		size_t ins = 0;
		const char *t = strrchr(f, '/');
		if (t && (size_t)(t - f) > ins)
			ins = (size_t)(t - f) + 1;
		t = strrchr(f, '\\');
		if (t && (size_t)(t - f) > ins)
			ins = (size_t)(t - f) + 1;
		char res[2400];
		snprintf(res, sizeof res, "%.*s%s%s", (int)ins, f, p, f + ins);
		snprintf(f, sizeof f, "%.1023s", res);
	}
	if (suffix && *suffix) {
		if (*suffix != ' ')
			strcat(f, " ");
		strcat(f, suffix);
	}
	remove_reserved(f);
	strcpy(out, f);
}

static const char *format_ext(const char *c)
{
	if (!strcmp(c, "fragmented_mp4") || !strcmp(c, "hybrid_mp4"))
		return "mp4";
	if (!strcmp(c, "fragmented_mov") || !strcmp(c, "hybrid_mov"))
		return "mov";
	if (!strcmp(c, "hls"))
		return "m3u8";
	if (!strcmp(c, "mpegts"))
		return "ts";
	return c;
}

static void find_best(char *path, bool nospace)
{
	if (!vexists(path))
		return;
	char *ext = strrchr(path, '.');
	if (!ext)
		return;
	size_t extStart = (size_t)(ext - path);
	int num = 2;
	for (;;) {
		char test[1280];
		char numstr[32];
		if (nospace)
			sprintf(numstr, "_%d", num++);
		else
			sprintf(numstr, " (%d)", num++);
		snprintf(test, sizeof test, "%.*s%s%s", (int)extStart, path, numstr, path + extStart);
		if (!vexists(test)) {
			strcpy(path, test);
			return;
		}
	}
}

/* GetOutputFilename; `stem` stands for the result of os_generate_formatted_filename minus the extension (S-023). */
static bool output_filename(const char *dir, const char *container, bool nospace, bool overwrite, const char *stem,
			    char *out)
{
	if (!dir || !*dir || !vdir(dir)) {
		out[0] = 0;
		return false;
	}
	char p[1280];
	snprintf(p, sizeof p, "%s", dir);
	char last = p[strlen(p) - 1];
	if (last != '/' && last != '\\')
		strcat(p, "/");
	strcat(p, stem);
	strcat(p, ".");
	strcat(p, format_ext(container));
	for (char *q = p; *q; q++) /* ensure_directory_exists normalises separators */
		if (*q == '\\')
			*q = '/';
	if (!overwrite)
		find_best(p, nospace);
	strcpy(out, p);
	return true;
}

/* ---------------- configs ---------------- */
static config_t *cfgs[3]; /* 0 app(global.ini) 1 user(user.ini) 2 profile(basic.ini) */
static const char *cfgname[3] = {"app", "user", "prof"};
static char tmpdir[400];

static int cfg_idx(const char *n)
{
	for (int i = 0; i < 3; i++)
		if (!strcmp(n, cfgname[i]))
			return i;
	fprintf(stderr, "bad config %s\n", n);
	exit(2);
}
static const char *snull(const char *s)
{
	return strcmp(s, "@null") ? s : NULL;
}

static void typed_set(config_t *c, bool def, const char *sec, const char *key, const char *type, const char *v)
{
	if (!strcmp(type, "string"))
		(def ? config_set_default_string : config_set_string)(c, sec, key, v);
	else if (!strcmp(type, "int"))
		(def ? config_set_default_int : config_set_int)(c, sec, key, strtoll(v, 0, 10));
	else if (!strcmp(type, "uint"))
		(def ? config_set_default_uint : config_set_uint)(c, sec, key, strtoull(v, 0, 10));
	else if (!strcmp(type, "bool"))
		(def ? config_set_default_bool : config_set_bool)(c, sec, key, !strcmp(v, "true") || !strcmp(v, "1"));
	else if (!strcmp(type, "double"))
		(def ? config_set_default_double : config_set_double)(c, sec, key, strtod(v, 0));
	else {
		fprintf(stderr, "bad type %s\n", type);
		exit(2);
	}
}

/* default tables, Linux column of the reference (see spec section 4) */
static bool g_print;
static void S(config_t *c, const char *sec, const char *k, const char *v)
{
	config_set_default_string(c, sec, k, v);
	if (g_print)
		printf("  %s.%s string \"%s\"\n", sec, k, v);
}
static void U(config_t *c, const char *sec, const char *k, uint64_t v)
{
	config_set_default_uint(c, sec, k, v);
	if (g_print)
		printf("  %s.%s uint %" PRIu64 "\n", sec, k, v);
}
static void I(config_t *c, const char *sec, const char *k, int64_t v)
{
	config_set_default_int(c, sec, k, v);
	if (g_print)
		printf("  %s.%s int %" PRId64 "\n", sec, k, v);
}
static void B(config_t *c, const char *sec, const char *k, bool v)
{
	config_set_default_bool(c, sec, k, v);
	if (g_print)
		printf("  %s.%s bool %s\n", sec, k, v ? "true" : "false");
}
static void D(config_t *c, const char *sec, const char *k, double v)
{
	config_set_default_double(c, sec, k, v);
	if (g_print)
		printf("  %s.%s double %.17g\n", sec, k, v);
}

static void app_defaults(config_t *c)
{
	U(c, "General", "MaxLogs", 10);
	I(c, "General", "InfoIncrement", -1);
	S(c, "General", "ProcessPriority", "Normal");
	B(c, "General", "EnableAutoUpdates", true);
	S(c, "Video", "Renderer", "OpenGL");
	B(c, "General", "BrowserHWAccel", true);
}

static void user_defaults(config_t *c)
{
	B(c, "General", "ConfirmOnExit", true);
	S(c, "General", "HotkeyFocusType", "NeverDisableHotkeys");
	B(c, "BasicWindow", "PreviewEnabled", true);
	B(c, "BasicWindow", "PreviewProgramMode", false);
	B(c, "BasicWindow", "SceneDuplicationMode", true);
	B(c, "BasicWindow", "SwapScenesMode", true);
	B(c, "BasicWindow", "SnappingEnabled", true);
	B(c, "BasicWindow", "ScreenSnapping", true);
	B(c, "BasicWindow", "SourceSnapping", true);
	B(c, "BasicWindow", "CenterSnapping", false);
	D(c, "BasicWindow", "SnapDistance", 5.0);
	B(c, "BasicWindow", "SpacingHelpersEnabled", true);
	B(c, "BasicWindow", "RecordWhenStreaming", false);
	B(c, "BasicWindow", "KeepRecordingWhenStreamStops", false);
	B(c, "BasicWindow", "SysTrayEnabled", true);
	B(c, "BasicWindow", "SysTrayWhenStarted", false);
	B(c, "BasicWindow", "SaveProjectors", false);
	B(c, "BasicWindow", "ShowTransitions", true);
	B(c, "BasicWindow", "ShowListboxToolbars", true);
	B(c, "BasicWindow", "ShowStatusBar", true);
	B(c, "BasicWindow", "ShowSourceIcons", true);
	B(c, "BasicWindow", "ShowContextToolbars", true);
	B(c, "BasicWindow", "StudioModeLabels", true);
	B(c, "BasicWindow", "SideDocks", true);
	B(c, "BasicWindow", "VerticalVolumeControl", true);
	B(c, "BasicWindow", "MultiviewMouseSwitch", true);
	B(c, "BasicWindow", "MultiviewDrawNames", true);
	B(c, "BasicWindow", "MultiviewDrawAreas", true);
	B(c, "BasicWindow", "MediaControlsCountdownTimer", true);
	B(c, "BasicWindow", "MixerShowInactive", false);
	B(c, "BasicWindow", "MixerKeepInactiveLast", false);
	B(c, "BasicWindow", "MixerShowHidden", false);
	B(c, "BasicWindow", "MixerKeepHiddenLast", false);
	I(c, "Appearance", "FontScale", 10);
	I(c, "Appearance", "Density", 1);
}

static const double scaled_vals[] = {1.0, 1.25, (1.0 / 0.75), 1.5, (1.0 / 0.6), 1.75, 2.0, 2.25, 2.5, 2.75, 3.0, 0.0};

/* the part of InitBasicConfigDefaults that sets defaults; `cx`,`cy` already decided (platform: Linux) */
static void basic_defaults(config_t *c, uint32_t cx, uint32_t cy, uint32_t *ocx, uint32_t *ocy)
{
	S(c, "Output", "Mode", "Simple");
	B(c, "Stream1", "IgnoreRecommended", false);
	B(c, "Stream1", "EnableMultitrackVideo", false);
	B(c, "Stream1", "MultitrackVideoMaximumAggregateBitrateAuto", true);
	B(c, "Stream1", "MultitrackVideoMaximumVideoTracksAuto", true);
	S(c, "SimpleOutput", "FilePath", "$HOME");
	S(c, "SimpleOutput", "RecFormat2", "hybrid_mp4");
	U(c, "SimpleOutput", "VBitrate", 6000);
	U(c, "SimpleOutput", "ABitrate", 160);
	B(c, "SimpleOutput", "UseAdvanced", false);
	S(c, "SimpleOutput", "Preset", "veryfast");
	S(c, "SimpleOutput", "NVENCPreset2", "p5");
	S(c, "SimpleOutput", "RecQuality", "Stream");
	B(c, "SimpleOutput", "RecRB", false);
	I(c, "SimpleOutput", "RecRBTime", 20);
	I(c, "SimpleOutput", "RecRBSize", 512);
	S(c, "SimpleOutput", "RecRBPrefix", "Replay");
	S(c, "SimpleOutput", "StreamAudioEncoder", "aac");
	S(c, "SimpleOutput", "RecAudioEncoder", "aac");
	U(c, "SimpleOutput", "RecTracks", 1);
	B(c, "AdvOut", "ApplyServiceSettings", true);
	B(c, "AdvOut", "UseRescale", false);
	U(c, "AdvOut", "TrackIndex", 1);
	U(c, "AdvOut", "VodTrackIndex", 2);
	S(c, "AdvOut", "Encoder", "obs_x264");
	S(c, "AdvOut", "RecType", "Standard");
	S(c, "AdvOut", "RecFilePath", "$HOME");
	S(c, "AdvOut", "RecFormat2", "hybrid_mp4");
	B(c, "AdvOut", "RecUseRescale", false);
	U(c, "AdvOut", "RecTracks", 1);
	S(c, "AdvOut", "RecEncoder", "none");
	U(c, "AdvOut", "FLVTrack", 1);
	U(c, "AdvOut", "StreamMultiTrackAudioMixes", 1);
	B(c, "AdvOut", "FFOutputToFile", true);
	S(c, "AdvOut", "FFFilePath", "$HOME");
	S(c, "AdvOut", "FFExtension", "mp4");
	U(c, "AdvOut", "FFVBitrate", 6000);
	U(c, "AdvOut", "FFVGOPSize", 250);
	B(c, "AdvOut", "FFUseRescale", false);
	B(c, "AdvOut", "FFIgnoreCompat", false);
	U(c, "AdvOut", "FFABitrate", 160);
	U(c, "AdvOut", "FFAudioMixes", 1);
	for (int t = 1; t <= 6; t++) {
		char k[32];
		sprintf(k, "Track%dBitrate", t);
		U(c, "AdvOut", k, 160);
	}
	U(c, "AdvOut", "RecSplitFileTime", 15);
	U(c, "AdvOut", "RecSplitFileSize", 2048);
	B(c, "AdvOut", "RecRB", false);
	U(c, "AdvOut", "RecRBTime", 20);
	I(c, "AdvOut", "RecRBSize", 512);
	U(c, "Video", "BaseCX", cx);
	U(c, "Video", "BaseCY", cy);
	S(c, "Output", "FilenameFormatting", "%CCYY-%MM-%DD %hh-%mm-%ss");
	B(c, "Output", "DelayEnable", false);
	U(c, "Output", "DelaySec", 20);
	B(c, "Output", "DelayPreserve", true);
	B(c, "Output", "Reconnect", true);
	U(c, "Output", "RetryDelay", 2);
	U(c, "Output", "MaxRetries", 25);
	S(c, "Output", "BindIP", "default");
	S(c, "Output", "IPFamily", "IPv4+IPv6");
	B(c, "Output", "NewSocketLoopEnable", false);
	B(c, "Output", "LowLatencyEnable", false);

	int i = 0;
	uint32_t scx = cx, scy = cy;
	while (((scx * scy) > (1280 * 720)) && scaled_vals[i] > 0.0) {
		double scale = scaled_vals[i++];
		scx = (uint32_t)((double)cx / scale);
		scy = (uint32_t)((double)cy / scale);
	}
	U(c, "Video", "OutputCX", scx);
	U(c, "Video", "OutputCY", scy);
	if (ocx)
		*ocx = scx;
	if (ocy)
		*ocy = scy;
	U(c, "Video", "FPSType", 0);
	S(c, "Video", "FPSCommon", "30");
	U(c, "Video", "FPSInt", 30);
	U(c, "Video", "FPSNum", 30);
	U(c, "Video", "FPSDen", 1);
	S(c, "Video", "ScaleType", "bicubic");
	S(c, "Video", "ColorFormat", "NV12");
	S(c, "Video", "ColorSpace", "709");
	S(c, "Video", "ColorRange", "Partial");
	U(c, "Video", "SdrWhiteLevel", 300);
	U(c, "Video", "HdrNominalPeakLevel", 1000);
	S(c, "Audio", "MonitoringDeviceId", "default");
	U(c, "Audio", "SampleRate", 48000);
	S(c, "Audio", "ChannelSetup", "Stereo");
	D(c, "Audio", "MeterDecayRate", 23.53);
	U(c, "Audio", "PeakMeterType", 0);
}

/* OBSBasic::InitBasicConfigDefaults: the migration part, executed before the defaults. */
static bool basic_migrate(config_t *a, config_t *user)
{
	bool changed = false;
	if (config_has_user_value(a, "AdvOut", "FFAudioTrack") && !config_has_user_value(a, "AdvOut", "Pre22.1Settings")) {
		int track = (int)config_get_int(a, "AdvOut", "FFAudioTrack");
		config_set_int(a, "AdvOut", "FFAudioMixes", 1LL << (track - 1));
		config_set_bool(a, "AdvOut", "Pre22.1Settings", true);
		changed = true;
	}
	if (config_has_user_value(a, "AdvOut", "RecTrackIndex") && !config_has_user_value(a, "AdvOut", "RecTracks")) {
		uint64_t track = config_get_uint(a, "AdvOut", "RecTrackIndex");
		track = 1ULL << (track - 1);
		config_set_uint(a, "AdvOut", "RecTracks", track);
		config_remove_value(a, "AdvOut", "RecTrackIndex");
		changed = true;
	}
	if (config_get_bool(user, "General", "Pre24.1Defaults") && !config_has_user_value(a, "Twitch", "AddonChoice")) {
		config_set_int(a, "Twitch", "AddonChoice", 3);
		changed = true;
	}
	if (config_has_user_value(a, "SimpleOutput", "EnforceBitrate") &&
	    !config_has_user_value(a, "Stream1", "IgnoreRecommended") &&
	    !config_has_user_value(a, "Stream1", "MovedOldEnforce")) {
		bool enforce = config_get_bool(a, "SimpleOutput", "EnforceBitrate");
		config_set_bool(a, "Stream1", "IgnoreRecommended", !enforce);
		config_set_bool(a, "Stream1", "MovedOldEnforce", true);
		changed = true;
	}
	if (config_has_user_value(a, "Output", "RetryDelay")) {
		int retryDelay = (int)config_get_uint(a, "Output", "RetryDelay");
		if (retryDelay < 1) {
			config_set_uint(a, "Output", "RetryDelay", 1);
			changed = true;
		}
	}
	const char *secs[2] = {"AdvOut", "SimpleOutput"};
	for (int s = 0; s < 2; s++) {
		const char *section = secs[s];
		bool has_old = config_has_user_value(a, section, "RecFormat");
		bool has_new = config_has_user_value(a, section, "RecFormat2");
		if (!has_new && !has_old)
			continue;
		char old_format[128];
		snprintf(old_format, sizeof old_format, "%s", config_get_string(a, section, has_new ? "RecFormat2" : "RecFormat"));
		const char *nf = old_format;
		if (!strcmp(old_format, "ts"))
			nf = "mpegts";
		else if (!strcmp(old_format, "m3u8"))
			nf = "hls";
		else if (!strcmp(old_format, "fmp4"))
			nf = "fragmented_mp4";
		else if (!strcmp(old_format, "fmov"))
			nf = "fragmented_mov";
		if (strcmp(nf, old_format) || !has_new) {
			config_set_string(a, section, "RecFormat2", nf);
			changed = true;
		}
	}
	if (config_get_bool(a, "AdvOut", "Rescale") && !config_has_user_value(a, "AdvOut", "RescaleFilter"))
		config_set_int(a, "AdvOut", "RescaleFilter", 3);
	if (config_get_bool(a, "AdvOut", "RecRescale") && !config_has_user_value(a, "AdvOut", "RecRescaleFilter"))
		config_set_int(a, "AdvOut", "RecRescaleFilter", 3);
	return changed;
}

/* OBSApp::MigrateLegacySettings. Returns (user changed, app changed) in bits 0/1. */
#define MAKE_VER(ma, mi, pa) (((uint32_t)(ma) << 24) | ((uint32_t)(mi) << 16) | (uint32_t)(pa))
static bool multiview_pre22(config_t *user, const char *layout)
{
	if (!layout)
		return false;
	static const struct {
		const char *n;
		int v;
	} t[] = {{"horizontaltop", 0}, {"horizontalbottom", 1}, {"verticalleft", 2}, {"verticalright", 3}};
	for (int i = 0; i < 4; i++)
		if (!strcasecmp(layout, t[i].n)) {
			config_set_int(user, "BasicWindow", "MultiviewLayout", t[i].v);
			return true;
		}
	return false;
}
static int migrate_legacy(config_t *app, config_t *user, uint32_t last)
{
	bool uc = false, gc = false;
	/* ordered map by version: 19, 21, 23, 24.1 */
	static const struct {
		uint32_t v;
		const char *k;
	} dm[4] = {{MAKE_VER(19, 0, 0), "Pre19Defaults"},
		   {MAKE_VER(21, 0, 0), "Pre21Defaults"},
		   {MAKE_VER(23, 0, 0), "Pre23Defaults"},
		   {MAKE_VER(24, 1, 0), "Pre24.1Defaults"}};
	for (int i = 0; i < 4; i++) {
		if (!config_has_user_value(user, "General", dm[i].k)) {
			bool old = last && last < dm[i].v;
			config_set_bool(user, "General", dm[i].k, old);
			uc = true;
		}
	}
	if (config_has_user_value(user, "BasicWindow", "MultiviewLayout")) {
		const char *layout = config_get_string(user, "BasicWindow", "MultiviewLayout");
		bool u = multiview_pre22(user, layout);
		uc = uc | u;
	}
	if (last && last < MAKE_VER(24, 0, 0)) {
		if (config_get_bool(user, "General", "DisableHotkeysInFocus"))
			config_set_string(user, "General", "HotkeyFocusType", "DisableHotkeysInFocus");
		uc = true;
	}
	if (last && last < MAKE_VER(33, 0, 0)) {
		bool migrated = config_has_user_value(app, "General", "Pre33.0Migrated");
		if (!migrated) {
			double cur = config_get_double(user, "BasicWindow", "SnapDistance");
			if (cur == 10.0)
				config_set_double(user, "BasicWindow", "SnapDistance", 5.0);
			config_set_bool(app, "General", "Pre33.0Migrated", true);
			uc = true;
			gc = true;
		}
	}
	return (uc ? 1 : 0) | (gc ? 2 : 0);
}

static void dump(config_t *c)
{
	char p[512];
	snprintf(p, sizeof p, "%s/fe.ini", tmpdir);
	config_t *t = config_create(p);
	(void)t;
	/* save the live config into the scratch path by round-tripping through a second object */
	size_t ns = config_num_sections(c);
	for (size_t i = 0; i < ns; i++)
		printf("section %s\n", config_get_section(c, i));
}

static void show(config_t *c, const char *sec, const char *key)
{
	const char *v = config_get_string(c, sec, key);
	printf("%s.%s user=%d default=%d str=", sec, key, config_has_user_value(c, sec, key),
	       config_has_default_value(c, sec, key));
	if (v)
		script_print_quoted(v);
	else
		printf("@null");
	printf("\n");
}

/* ---------------- CLI (obs-main.cpp) ---------------- */
static bool arg_is(const char *arg, const char *lf, const char *sf)
{
	return (lf && !strcmp(arg, lf)) || (sf && !strcmp(arg, sf));
}

static void cli(int argc, char **argv, bool portable_allowed)
{
	bool multi = 0, portable = 0, verbose = 0, safe = 0, only_bundled = 0, aot = 0, unfiltered = 0, ss = 0, sr = 0,
	     srb = 0, svc = 0, tray = 0, studio = 0, gl = 0, noupd = 0, nomiss = 0, steam = 0;
	const char *coll = "", *prof = "", *scene = "";
	for (int i = 1; i < argc; i++) {
		if (arg_is(argv[i], "--multi", "-m")) {
			multi = 1;
		} else if (portable_allowed && arg_is(argv[i], "--portable", "-p")) {
			portable = 1;
		} else if (arg_is(argv[i], "--verbose", NULL)) {
			verbose = 1;
		} else if (arg_is(argv[i], "--safe-mode", NULL)) {
			safe = 1;
		} else if (arg_is(argv[i], "--only-bundled-plugins", NULL)) {
			only_bundled = 1;
		} else if (arg_is(argv[i], "--always-on-top", NULL)) {
			aot = 1;
		} else if (arg_is(argv[i], "--unfiltered_log", NULL)) {
			unfiltered = 1;
		} else if (arg_is(argv[i], "--startstreaming", NULL)) {
			ss = 1;
		} else if (arg_is(argv[i], "--startrecording", NULL)) {
			sr = 1;
		} else if (arg_is(argv[i], "--startreplaybuffer", NULL)) {
			srb = 1;
		} else if (arg_is(argv[i], "--startvirtualcam", NULL)) {
			svc = 1;
		} else if (arg_is(argv[i], "--collection", NULL)) {
			if (++i < argc)
				coll = argv[i];
		} else if (arg_is(argv[i], "--profile", NULL)) {
			if (++i < argc)
				prof = argv[i];
		} else if (arg_is(argv[i], "--scene", NULL)) {
			if (++i < argc)
				scene = argv[i];
		} else if (arg_is(argv[i], "--minimize-to-tray", NULL)) {
			tray = 1;
		} else if (arg_is(argv[i], "--studio-mode", NULL)) {
			studio = 1;
		} else if (arg_is(argv[i], "--allow-opengl", NULL)) {
			gl = 1;
		} else if (arg_is(argv[i], "--disable-updater", NULL)) {
			noupd = 1;
		} else if (arg_is(argv[i], "--disable-missing-files-check", NULL)) {
			nomiss = 1;
		} else if (arg_is(argv[i], "--steam", NULL)) {
			steam = 1;
		} else if (arg_is(argv[i], "--help", "-h")) {
			printf("exit help\n");
			return;
		} else if (arg_is(argv[i], "--version", "-V")) {
			printf("exit version\n");
			return;
		}
	}
	printf("multi=%d portable=%d verbose=%d safe_mode=%d only_bundled=%d always_on_top=%d unfiltered_log=%d\n", multi,
	       portable, verbose, safe, only_bundled, aot, unfiltered);
	printf("start_streaming=%d start_recording=%d start_replaybuffer=%d start_virtualcam=%d\n", ss, sr, srb, svc);
	printf("minimize_tray=%d studio_mode=%d allow_opengl=%d disable_updater=%d disable_missing_files_check=%d steam=%d\n",
	       tray, studio, gl, noupd, nomiss, steam);
	printf("collection=\"%s\" profile=\"%s\" scene=\"%s\"\n", coll, prof, scene);
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;
	const char *d = getenv("CONFORMANCE_TMPDIR");
	snprintf(tmpdir, sizeof tmpdir, "%s", d ? d : ".");
	for (int i = 0; i < 3; i++)
		config_open_string(&cfgs[i], "");

	while (script_next(f, &sl, &lineno)) {
		const char *cmd = sl.argv[0];
		if (!strcmp(cmd, "cli")) {
			/* cli PORTABLE_ALLOWED args... */
			script_need(&sl, 2);
			char *av[SCRIPT_MAX_TOKENS];
			av[0] = "obs";
			int n = 1;
			for (int i = 2; i < sl.argc; i++)
				av[n++] = sl.argv[i];
			printf("cli %s\n", sl.argv[1]);
			cli(n, av, !strcmp(sl.argv[1], "1"));
		} else if (!strcmp(cmd, "fs_add")) {
			vfiles[nvfiles++] = strdup(sl.argv[1]);
		} else if (!strcmp(cmd, "dir_add")) {
			vdirs[nvdirs++] = strdup(sl.argv[1]);
		} else if (!strcmp(cmd, "safe_name")) {
			char o[1100];
			file_safe_name(sl.argv[1], o);
			printf("safe_name -> \"%s\"\n", o);
		} else if (!strcmp(cmd, "unused")) {
			script_need(&sl, 2);
			char p[1100];
			snprintf(p, sizeof p, "%s", sl.argv[1]);
			closest_unused(p, sl.argc > 2 ? snull(sl.argv[2]) : NULL);
			printf("unused -> \"%s\"\n", p);
		} else if (!strcmp(cmd, "fmtstr")) {
			script_need(&sl, 4);
			char o[1400];
			format_string(sl.argv[1], snull(sl.argv[2]), snull(sl.argv[3]), o);
			printf("fmtstr -> \"%s\"\n", o);
		} else if (!strcmp(cmd, "ext")) {
			printf("ext -> \"%s\"\n", format_ext(sl.argv[1]));
		} else if (!strcmp(cmd, "best")) {
			char p[1400];
			snprintf(p, sizeof p, "%s", sl.argv[1]);
			find_best(p, !strcmp(sl.argv[2], "1"));
			printf("best -> \"%s\"\n", p);
		} else if (!strcmp(cmd, "outname")) {
			/* outname DIR CONTAINER NOSPACE OVERWRITE STEM */
			script_need(&sl, 6);
			char o[1500];
			bool ok = output_filename(sl.argv[1], sl.argv[2], !strcmp(sl.argv[3], "1"), !strcmp(sl.argv[4], "1"),
						  sl.argv[5], o);
			printf("outname ok=%d -> \"%s\"\n", ok, o);
		} else if (!strcmp(cmd, "set") || !strcmp(cmd, "setdef")) {
			script_need(&sl, 6);
			typed_set(cfgs[cfg_idx(sl.argv[1])], !strcmp(cmd, "setdef"), sl.argv[2], sl.argv[3], sl.argv[4],
				  sl.argv[5]);
		} else if (!strcmp(cmd, "unset")) {
			config_remove_value(cfgs[cfg_idx(sl.argv[1])], sl.argv[2], sl.argv[3]);
		} else if (!strcmp(cmd, "show")) {
			script_need(&sl, 4);
			printf("%s: ", sl.argv[1]);
			show(cfgs[cfg_idx(sl.argv[1])], sl.argv[2], sl.argv[3]);
		} else if (!strcmp(cmd, "defaults")) {
			/* defaults app|user|basic [CX CY] : print every default as "S.K=string" */
			config_t *c;
			config_open_string(&c, "");
			uint32_t ocx = 0, ocy = 0;
			printf("defaults %s\n", sl.argv[1]);
			g_print = true;
			if (!strcmp(sl.argv[1], "app"))
				app_defaults(c);
			else if (!strcmp(sl.argv[1], "user"))
				user_defaults(c);
			else
				basic_defaults(c, (uint32_t)atoi(sl.argv[2]), (uint32_t)atoi(sl.argv[3]), &ocx, &ocy);
			g_print = false;
			config_close(c);
		} else if (!strcmp(cmd, "migrate_legacy")) {
			int r = migrate_legacy(cfgs[0], cfgs[1], (uint32_t)strtoul(sl.argv[1], 0, 10));
			printf("migrate_legacy -> user_saved=%d app_saved=%d\n", r & 1, !!(r & 2));
		} else if (!strcmp(cmd, "basic_migrate")) {
			printf("basic_migrate -> changed=%d\n", basic_migrate(cfgs[2], cfgs[1]));
		} else if (!strcmp(cmd, "resdef")) {
			/* resdef W H PRE19 : default Base/Output resolution for monitor size W x H */
			script_need(&sl, 4);
			uint32_t cx = (uint32_t)atoi(sl.argv[1]), cy = (uint32_t)atoi(sl.argv[2]);
			bool old = !strcmp(sl.argv[3], "1");
			if (!old && (cx * cy) > (1920 * 1080)) {
				cx = 1920;
				cy = 1080;
			}
			config_t *c;
			config_open_string(&c, "");
			uint32_t ox, oy;
			basic_defaults(c, cx, cy, &ox, &oy);
			printf("resdef base=%ux%u out=%ux%u\n", cx, cy, ox, oy);
			config_close(c);
		} else if (!strcmp(cmd, "pre31")) {
			/* pre31 LASTVER USER_INI_EXISTS COPY_OK : decision of InitGlobalConfig / MigrateGlobalSettings */
			uint32_t last = (uint32_t)strtoul(sl.argv[1], 0, 10);
			bool exists = !strcmp(sl.argv[2], "1"), copyok = !strcmp(sl.argv[3], "1");
			if (last && last < MAKE_VER(31, 0, 0)) {
				bool already = config_get_bool(cfgs[0], "General", "Pre31Migrated");
				if (!already) {
					bool migrated = !exists && copyok;
					config_set_bool(cfgs[0], "General", "Pre31Migrated", migrated);
					printf("pre31 attempted migrated=%d\n", migrated);
				} else
					printf("pre31 skipped (already flagged)\n");
			} else
				printf("pre31 not applicable\n");
		} else if (!strcmp(cmd, "multiview_pre22")) {
			bool r = multiview_pre22(cfgs[1], snull(sl.argv[1]));
			printf("multiview_pre22 -> %d\n", r);
		} else if (!strcmp(cmd, "sections")) {
			dump(cfgs[cfg_idx(sl.argv[1])]);
		} else if (!strcmp(cmd, "reset")) {
			for (int i = 0; i < 3; i++) {
				config_close(cfgs[i]);
				config_open_string(&cfgs[i], "");
			}
			nvfiles = nvdirs = 0;
		} else if (!strcmp(cmd, "echo")) {
			printf("%s\n", sl.argc > 1 ? sl.argv[1] : "");
		} else {
			fprintf(stderr, "unknown command %s at %d\n", cmd, lineno);
			return 2;
		}
	}
	return 0;
}
