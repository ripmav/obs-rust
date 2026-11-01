/* Conformance harness (area imagesource, spec S-030): interface between runner and fake host. */
#pragma once
#include <obs.h>
#include <graphics/image-file.h>

struct fake_src; /* == struct obs_source of the fake host */

extern uint64_t host_clock_ns;
extern int host_srgb_fb, host_linear_srgb, host_task_auto;
extern uint32_t host_base_w, host_base_h;

void host_reset_rand(const int *v, int n);
void host_fs_file(const char *path, long long mtime);
void host_fs_rm(const char *path);
void host_fs_dir(const char *path, int n, char **entries);
void host_img_still(const char *path, int w, int h);
void host_img_anim(const char *path, int w, int h, int loop_max, int filesize, int n, const int *delays);

const struct obs_source_info *host_find_info(const char *id);
int host_num_infos(void);
const struct obs_source_info *host_info_at(int i);
obs_source_t *host_find(const char *name);
obs_source_t *host_nth(int serial);
int host_count(void);
void host_tick_all(float seconds);
void host_run_tasks(void);
void host_hotkey(obs_source_t *s, const char *name, bool pressed);
void host_media(obs_source_t *s, const char *what);
const char *imagesource_label(obs_source_t *s);
void host_set_refs(obs_source_t *s, int show, int act);
void host_render(obs_source_t *s);
void host_connect(obs_source_t *s, const char *sig);
void host_proc(obs_source_t *s, const char *name);
void host_dump(obs_source_t *s);
void host_missing(obs_source_t *s, int idx, const char *newpath);
void host_props(obs_source_t *s);
void host_defaults(const struct obs_source_info *i);
void host_size(obs_source_t *s);
void host_colorspace(obs_source_t *s);
void host_info_print(const struct obs_source_info *i);
void host_update(obs_source_t *s, obs_data_t *d);
void host_media_state(obs_source_t *s);
void host_tr_print(obs_source_t *s);
const char *host_name(obs_source_t *s);

void imagesource_img_dump(void *);
void imagesource_color_dump(void *);
void imagesource_ss1_dump(void *);
void imagesource_ss2_dump(void *);
bool obs_module_load(void);
extern struct obs_source_info slideshow_info, slideshow_info_mk2;
const struct obs_source_info *host_info_of(obs_source_t *s);
signal_handler_t *host_sh(obs_source_t *s);
proc_handler_t *host_ph(obs_source_t *s);
void *host_data(obs_source_t *s);
obs_source_t *host_create(const char *name, const char *id, obs_data_t *settings);
const char *host_reg_id(int i);
void host_add_rand(const int *v, int n);
