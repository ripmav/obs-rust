/*
 * Conformance harness (area rtmpservices, spec S-026): recording fakes for everything the
 * plugins/rtmp-services sources use from the rest of OBS.
 *
 *   - an in-memory flat file system (os_quick_read/write, os_file_exists, os_safe_replace, ...)
 *   - module data/config directory resolution (obs_find_module_file, obs_module_get_config_path)
 *   - a tiny locale table (the en-US strings of plugins/rtmp-services/data/locale/en-US.ini)
 *   - the protocol registry (obs_is_output_protocol_registered), obs_get_video_info,
 *     obs_get_version_string, obs_get_proc_handler/proc_handler_add, obs_register_service_s
 *   - a fake clock (os_gettime_ns, os_sleep_ms, time) so the "wait for ingests" loops are instant
 *   - the file-updater API (update_info_*): records the request and delivers scripted responses
 *   - a fake libcurl (curl_easy_*): records the request and answers from scripted routes
 *
 * Everything observable prints one line starting with "fake ". The runner controls the fakes
 * through the fake_* functions declared at the top of rtmpservices_runner.c.
 * Test tooling, not part of libobs.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <curl/curl.h>

#include <obs.h>
#include <file-updater/file-updater.h>
#include <util/platform.h>
#include <util/text-lookup.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <callback/proc.h>

#undef curl_easy_setopt
#undef curl_easy_getinfo

/* ------------------------------------------------------------------------- */
/* in-memory file system                                                      */

#define FS_MAX 64
static struct {
	char *path;
	char *data;
	size_t len;
} fs_entries[FS_MAX];

char *fake_data_dir;   /* NULL: the module has no data directory */
char *fake_config_dir; /* NULL: no config path available */
static bool fs_inited;

static void fs_init(void)
{
	if (fs_inited)
		return;
	fs_inited = true;
	fake_data_dir = strdup("/mod/data");
	fake_config_dir = strdup("/cfg/rtmp-services/");
}

static int fs_find(const char *path)
{
	for (int i = 0; i < FS_MAX; i++)
		if (fs_entries[i].path && strcmp(fs_entries[i].path, path) == 0)
			return i;
	return -1;
}

void fake_fs_put(const char *path, const char *data, size_t len)
{
	fs_init();
	int i = fs_find(path);
	if (i < 0) {
		for (i = 0; i < FS_MAX && fs_entries[i].path; i++)
			;
		if (i == FS_MAX) {
			fprintf(stderr, "fake fs full\n");
			exit(2);
		}
		fs_entries[i].path = strdup(path);
	} else {
		free(fs_entries[i].data);
	}
	fs_entries[i].data = malloc(len + 1);
	memcpy(fs_entries[i].data, data, len);
	fs_entries[i].data[len] = 0;
	fs_entries[i].len = len;
}

bool fake_fs_get(const char *path, const char **data, size_t *len)
{
	int i = fs_find(path);
	if (i < 0)
		return false;
	*data = fs_entries[i].data;
	*len = fs_entries[i].len;
	return true;
}

void fake_fs_del(const char *path)
{
	int i = fs_find(path);
	if (i < 0)
		return;
	free(fs_entries[i].path);
	free(fs_entries[i].data);
	fs_entries[i].path = NULL;
	fs_entries[i].data = NULL;
}

static int cmp_str(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

void fake_fs_list(void)
{
	const char *names[FS_MAX];
	int n = 0;
	for (int i = 0; i < FS_MAX; i++)
		if (fs_entries[i].path)
			names[n++] = fs_entries[i].path;
	qsort(names, (size_t)n, sizeof(names[0]), cmp_str);
	for (int i = 0; i < n; i++) {
		int k = fs_find(names[i]);
		printf("fs %s len=%zu\n", names[i], fs_entries[k].len);
	}
}

bool os_file_exists(const char *path)
{
	fs_init();
	if (!path)
		return false;
	if (fake_data_dir && strcmp(path, fake_data_dir) == 0)
		return true;
	return fs_find(path) >= 0;
}

/* os_fread_utf8 semantics: a leading UTF-8 BOM is dropped; an empty file yields NULL. */
char *os_quick_read_utf8_file(const char *path)
{
	fs_init();
	int i = fs_find(path);
	if (i < 0)
		return NULL;
	const char *d = fs_entries[i].data;
	size_t len = fs_entries[i].len;
	if (len >= 3 && (unsigned char)d[0] == 0xEF && (unsigned char)d[1] == 0xBB && (unsigned char)d[2] == 0xBF) {
		d += 3;
		len -= 3;
	}
	if (!len)
		return NULL;
	char *out = bmalloc(len + 1);
	memcpy(out, d, len);
	out[len] = 0;
	return out;
}

bool os_quick_write_utf8_file(const char *path, const char *str, size_t len, bool marker)
{
	fs_init();
	if (marker) {
		char *tmp = malloc(len + 4);
		memcpy(tmp, "\xEF\xBB\xBF", 3);
		memcpy(tmp + 3, str, len);
		fake_fs_put(path, tmp, len + 3);
		free(tmp);
	} else {
		fake_fs_put(path, str, len);
	}
	printf("fake write %s len=%zu\n", path, len);
	return true;
}

bool os_quick_write_utf8_file_safe(const char *path, const char *str, size_t len, bool marker, const char *temp_ext,
				   const char *backup_ext)
{
	(void)temp_ext, (void)backup_ext;
	return os_quick_write_utf8_file(path, str, len, marker);
}

int os_safe_replace(const char *target_path, const char *from_path, const char *backup_path)
{
	fs_init();
	int f = fs_find(from_path);
	if (f < 0)
		return -1;
	int t = fs_find(target_path);
	if (backup_path && t >= 0)
		fake_fs_put(backup_path, fs_entries[t].data, fs_entries[t].len);
	fake_fs_put(target_path, fs_entries[f].data, fs_entries[f].len);
	fake_fs_del(from_path);
	printf("fake replace %s\n", target_path);
	return 0;
}

int os_rename(const char *old_path, const char *new_path)
{
	fs_init();
	int f = fs_find(old_path);
	if (f < 0)
		return -1;
	fake_fs_put(new_path, fs_entries[f].data, fs_entries[f].len);
	fake_fs_del(old_path);
	return 0;
}

/* ------------------------------------------------------------------------- */
/* module paths and locale                                                    */

char *obs_find_module_file(obs_module_t *module, const char *file)
{
	(void)module;
	fs_init();
	if (!file)
		file = "";
	if (!fake_data_dir)
		return NULL;
	struct dstr out = {0};
	dstr_copy(&out, fake_data_dir);
	if (!dstr_is_empty(&out) && dstr_end(&out) != '/' && *file)
		dstr_cat_ch(&out, '/');
	dstr_cat(&out, file);
	if (!os_file_exists(out.array))
		dstr_free(&out);
	return out.array;
}

char *obs_module_get_config_path(obs_module_t *module, const char *file)
{
	(void)module;
	fs_init();
	if (!fake_config_dir)
		return NULL;
	struct dstr out = {0};
	dstr_copy(&out, fake_config_dir);
	dstr_cat(&out, file);
	return out.array;
}

static const struct {
	const char *key, *val;
} locale_table[] = {
	{"StreamingServices", "Streaming Services"},
	{"CustomStreamingServer", "Custom Streaming Server"},
	{"Service", "Service"},
	{"Server", "Server"},
	{"Server.Auto", "Auto (Recommended)"},
	{"Server.AutoRTMPS", "Auto (RTMPS, Recommended)"},
	{"Server.AutoRTMP", "Auto (RTMP)"},
	{"StreamKey", "Stream key"},
	{"UseAuth", "Use authentication"},
	{"Username", "Username"},
	{"Password", "Password"},
	{"ShowAll", "Show all services"},
	{"MultitrackVideo.Disclaimer",
	 "%1 automatically optimizes your stream settings, which may include encoding multiple video qualities "
	 "and/or canvas orientations. Selecting this option will send %2 information about your computer and "
	 "software setup."},
	{"MultitrackVideo.LearnMoreLink", " <a href='%1'>Learn More</a>"},
};

static char locale_marker;

lookup_t *obs_module_load_locale(obs_module_t *module, const char *default_locale, const char *locale)
{
	(void)module, (void)default_locale, (void)locale;
	return (lookup_t *)&locale_marker;
}

void text_lookup_destroy(lookup_t *lookup)
{
	(void)lookup;
}

bool text_lookup_getstr(lookup_t *lookup, const char *lookup_val, const char **out)
{
	if (!lookup)
		return false;
	for (size_t i = 0; i < sizeof(locale_table) / sizeof(locale_table[0]); i++) {
		if (strcmp(locale_table[i].key, lookup_val) == 0) {
			*out = locale_table[i].val;
			return true;
		}
	}
	return false;
}

/* ------------------------------------------------------------------------- */
/* protocols, video info, version, proc handler, service registry             */

#define MAX_PROTOCOLS 8
char *fake_protocols[MAX_PROTOCOLS] = {"RTMP", "RTMPS"};
int fake_protocol_count = 2;
bool fake_video_valid;
struct obs_video_info fake_video;

bool obs_is_output_protocol_registered(const char *protocol)
{
	for (int i = 0; i < fake_protocol_count; i++)
		if (strcmp(protocol, fake_protocols[i]) == 0)
			return true;
	return false;
}

bool obs_get_video_info(struct obs_video_info *ovi)
{
	if (!fake_video_valid)
		return false;
	*ovi = fake_video;
	return true;
}

const char *obs_get_version_string(void)
{
	return "99.0.0-test";
}

struct proc_handler {
	int dummy;
};
static struct proc_handler fake_ph;

#define MAX_PROCS 8
struct fake_proc {
	char decl[128];
	proc_handler_proc_t proc;
	void *data;
};
struct fake_proc fake_procs[MAX_PROCS];
int fake_proc_count;

proc_handler_t *obs_get_proc_handler(void)
{
	return &fake_ph;
}

void proc_handler_add(proc_handler_t *handler, const char *decl_string, proc_handler_proc_t proc, void *data)
{
	(void)handler;
	if (fake_proc_count < MAX_PROCS) {
		snprintf(fake_procs[fake_proc_count].decl, sizeof(fake_procs[0].decl), "%s", decl_string);
		fake_procs[fake_proc_count].proc = proc;
		fake_procs[fake_proc_count].data = data;
		fake_proc_count++;
	}
	printf("fake proc_handler_add \"%s\"\n", decl_string);
}

struct obs_service_info fake_registered[4];
int fake_registered_count;

void obs_register_service_s(const struct obs_service_info *info, size_t size)
{
	(void)size;
	if (fake_registered_count < 4)
		fake_registered[fake_registered_count++] = *info;
	printf("fake obs_register_service id=%s\n", info->id);
}

/* ------------------------------------------------------------------------- */
/* clock                                                                      */

uint64_t fake_now_ns;
static void deliver_due(void);

uint64_t os_gettime_ns(void)
{
	return fake_now_ns;
}

void fake_advance_ms(uint64_t ms)
{
	fake_now_ns += ms * 1000000ULL;
	deliver_due();
}

void os_sleep_ms(uint32_t duration)
{
	fake_advance_ms(duration);
}

/* libc override: nimotv.c uses time()/difftime() directly. */
time_t time(time_t *t)
{
	time_t v = (time_t)(1700000000 + fake_now_ns / 1000000000ULL);
	if (t)
		*t = v;
	return v;
}

/* ------------------------------------------------------------------------- */
/* file-updater                                                               */

#define MAX_UROUTES 16
static struct {
	char *url;
	uint64_t delay_ms;
	char *body;
} uroutes[MAX_UROUTES];

struct update_info {
	int kind; /* 0 = package updater, 1 = single file */
	char *url;
	confirm_file_callback_t cb;
	void *param;
	bool pending;
	uint64_t due_ns;
	char *body;
	bool alive;
};

confirm_file_callback_t fake_confirm_cb; /* callback given to update_info_create (package updater) */
void *fake_confirm_param;

void fake_updater_route(const char *url, uint64_t delay_ms, const char *body)
{
	for (int i = 0; i < MAX_UROUTES; i++) {
		if (!uroutes[i].url || strcmp(uroutes[i].url, url) == 0) {
			free(uroutes[i].url);
			free(uroutes[i].body);
			uroutes[i].url = strdup(url);
			uroutes[i].delay_ms = delay_ms;
			uroutes[i].body = strdup(body);
			return;
		}
	}
	fprintf(stderr, "fake updater routes full\n");
	exit(2);
}

#define MAX_INFOS 64
static struct update_info *infos[MAX_INFOS];

static void deliver(struct update_info *ui)
{
	struct file_download_data fd = {0};
	size_t len = strlen(ui->body);
	fd.name = "single";
	da_resize(fd.buffer, len + 1);
	memcpy(fd.buffer.array, ui->body, len + 1);
	fd.buffer.num = len;
	ui->pending = false;
	printf("fake updater-deliver %s len=%zu\n", ui->url, len);
	ui->cb(ui->param, &fd);
	da_free(fd.buffer);
}

static void deliver_due(void)
{
	for (int i = 0; i < MAX_INFOS; i++) {
		struct update_info *ui = infos[i];
		if (ui && ui->alive && ui->pending && ui->due_ns <= fake_now_ns)
			deliver(ui);
	}
}

static struct update_info *register_info(struct update_info *ui)
{
	ui->alive = true;
	for (int i = 0; i < MAX_INFOS; i++) {
		if (!infos[i]) {
			infos[i] = ui;
			return ui;
		}
	}
	fprintf(stderr, "fake updater table full\n");
	exit(2);
}

update_info_t *update_info_create(const char *log_prefix, const char *user_agent, const char *update_url,
				  const char *local_dir, const char *cache_dir, confirm_file_callback_t confirm_callback,
				  void *param)
{
	struct update_info *ui = calloc(1, sizeof(*ui));
	ui->kind = 0;
	ui->url = strdup(update_url ? update_url : "");
	ui->cb = confirm_callback;
	ui->param = param;
	fake_confirm_cb = confirm_callback;
	fake_confirm_param = param;
	printf("fake update_info_create prefix=\"%s\" agent=\"%s\" url=%s local=%s cache=%s confirm=%s\n",
	       log_prefix ? log_prefix : "@null", user_agent ? user_agent : "@null", update_url,
	       local_dir ? local_dir : "@null", cache_dir ? cache_dir : "@null",
	       confirm_callback ? "set" : "@null");
	return register_info(ui);
}

update_info_t *update_info_create_single(const char *log_prefix, const char *user_agent, const char *file_url,
					 confirm_file_callback_t confirm_callback, void *param)
{
	struct update_info *ui = calloc(1, sizeof(*ui));
	ui->kind = 1;
	ui->url = strdup(file_url ? file_url : "");
	ui->cb = confirm_callback;
	ui->param = param;
	printf("fake update_info_create_single prefix=\"%s\" agent=\"%s\" url=%s\n", log_prefix ? log_prefix : "@null",
	       user_agent ? user_agent : "@null", file_url ? file_url : "@null");
	register_info(ui);
	for (int i = 0; i < MAX_UROUTES; i++) {
		if (uroutes[i].url && strcmp(uroutes[i].url, file_url) == 0) {
			ui->body = strdup(uroutes[i].body);
			ui->due_ns = fake_now_ns + uroutes[i].delay_ms * 1000000ULL;
			ui->pending = true;
			if (uroutes[i].delay_ms == 0)
				deliver(ui);
			break;
		}
	}
	return ui;
}

void update_info_destroy(update_info_t *info)
{
	if (!info)
		return;
	printf("fake update_info_destroy url=%s pending=%d\n", info->url, info->pending ? 1 : 0);
	info->alive = false;
	info->pending = false;
	/* the record is intentionally leaked into the table (kept for the process lifetime) */
}

/* ------------------------------------------------------------------------- */
/* libcurl                                                                    */

#define MAX_CROUTES 16
static struct {
	char *url;
	long status; /* 0: perform fails (CURLE_COULDNT_CONNECT) */
	char *body;
} croutes[MAX_CROUTES];

void fake_curl_route(const char *url, long status, const char *body)
{
	for (int i = 0; i < MAX_CROUTES; i++) {
		if (!croutes[i].url || strcmp(croutes[i].url, url) == 0) {
			free(croutes[i].url);
			free(croutes[i].body);
			croutes[i].url = strdup(url);
			croutes[i].status = status;
			croutes[i].body = strdup(body);
			return;
		}
	}
	fprintf(stderr, "fake curl routes full\n");
	exit(2);
}

void fake_curl_clear(void)
{
	for (int i = 0; i < MAX_CROUTES; i++) {
		free(croutes[i].url);
		free(croutes[i].body);
		croutes[i].url = NULL;
		croutes[i].body = NULL;
	}
}

struct fake_curl {
	char *url;
	size_t (*write_fn)(void *, size_t, size_t, void *);
	void *write_data;
	long timeout, verifypeer, verifyhost;
	long status;
};

CURL *curl_easy_init(void)
{
	return calloc(1, sizeof(struct fake_curl));
}

void curl_easy_cleanup(CURL *h)
{
	struct fake_curl *c = h;
	if (!c)
		return;
	free(c->url);
	free(c);
}

CURLcode curl_easy_setopt(CURL *h, CURLoption opt, ...)
{
	struct fake_curl *c = h;
	va_list va;
	va_start(va, opt);
	switch (opt) {
	case CURLOPT_URL:
		free(c->url);
		{
			const char *u = va_arg(va, char *);
			c->url = u ? strdup(u) : NULL; /* NULL clears the url like libcurl */
		}
		break;
	case CURLOPT_WRITEFUNCTION:
		c->write_fn = va_arg(va, size_t (*)(void *, size_t, size_t, void *));
		break;
	case CURLOPT_WRITEDATA:
		c->write_data = va_arg(va, void *);
		break;
	case CURLOPT_TIMEOUT:
		c->timeout = va_arg(va, long);
		break;
	case CURLOPT_SSL_VERIFYPEER:
		c->verifypeer = va_arg(va, long);
		break;
	case CURLOPT_SSL_VERIFYHOST:
		c->verifyhost = va_arg(va, long);
		break;
	default:
		break;
	}
	va_end(va);
	return CURLE_OK;
}

CURLcode curl_easy_perform(CURL *h)
{
	struct fake_curl *c = h;
	if (!c->url) {
		printf("fake curl GET @null -> url-malformat\n");
		return CURLE_URL_MALFORMAT;
	}
	printf("fake curl GET %s timeout=%ld verifypeer=%ld verifyhost=%ld\n", c->url, c->timeout, c->verifypeer,
	       c->verifyhost);
	for (int i = 0; i < MAX_CROUTES; i++) {
		if (croutes[i].url && strcmp(croutes[i].url, c->url) == 0) {
			if (croutes[i].status == 0) {
				printf("fake curl -> connect-failure\n");
				return CURLE_COULDNT_CONNECT;
			}
			c->status = croutes[i].status;
			const char *b = croutes[i].body;
			size_t len = strlen(b), off = 0;
			/* deliver in 5-byte chunks to exercise accumulation */
			while (off < len) {
				size_t n = len - off < 5 ? len - off : 5;
				if (c->write_fn && c->write_fn((void *)(b + off), 1, n, c->write_data) != n)
					return CURLE_WRITE_ERROR;
				off += n;
			}
			printf("fake curl -> status=%ld bytes=%zu\n", c->status, len);
			return CURLE_OK;
		}
	}
	printf("fake curl -> no-route\n");
	return CURLE_COULDNT_CONNECT;
}

CURLcode curl_easy_getinfo(CURL *h, CURLINFO info, ...)
{
	struct fake_curl *c = h;
	va_list va;
	va_start(va, info);
	if (info == CURLINFO_RESPONSE_CODE) {
		long *out = va_arg(va, long *);
		*out = c->status;
	}
	va_end(va);
	return CURLE_OK;
}

const char *curl_easy_strerror(CURLcode code)
{
	(void)code;
	return "fake curl error";
}

/* RFC 3986 unreserved characters stay, everything else is %XX (uppercase hex). */
char *curl_easy_escape(CURL *h, const char *string, int length)
{
	(void)h;
	size_t n = length ? (size_t)length : strlen(string);
	char *out = malloc(n * 3 + 1), *p = out;
	for (size_t i = 0; i < n; i++) {
		unsigned char ch = (unsigned char)string[i];
		if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' ||
		    ch == '.' || ch == '_' || ch == '~')
			*p++ = (char)ch;
		else
			p += sprintf(p, "%%%02X", ch);
	}
	*p = 0;
	return out;
}

void curl_free(void *p)
{
	free(p);
}
