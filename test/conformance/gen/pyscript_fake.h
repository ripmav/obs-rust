/* Shared declarations between pyscript_runner.c and pyscript_stubs.c (fake libobs for the Python backend). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <obs.h>
#include <obs-source.h>
#include <obs-frontend-api.h>

#define LF_MAX_TYPES 8
#define LF_MAX_CB 32
#define LF_MAX_SRC 8

struct fake_type {
	struct obs_source_info info;
	bool enabled;
	bool used;
};

struct obs_source { /* fake */
	struct fake_type *type;
	obs_data_t *settings;
	char name[32];
	void *data;
	int refs;
	struct obs_source *filters[4];
	int nfilters;
	bool in_enum;
};
struct obs_scene { /* fake */
	int nitems;
};
struct obs_scene_item { /* fake */
	int id;
	int refs;
};

struct lf_tick {
	void (*fn)(void *, float);
	void *p;
};
struct lf_render {
	void (*fn)(void *, uint32_t, uint32_t);
	void *p;
};
struct lf_hotkey {
	obs_hotkey_func func;
	void *data;
	char name[32];
	bool live;
};

extern struct fake_type lf_types[LF_MAX_TYPES];
extern struct obs_source lf_sources[LF_MAX_SRC];
extern int lf_nsources;
extern struct obs_scene lf_scene;
extern struct obs_scene_item lf_items[4];
extern uint64_t lf_video_time;
extern struct lf_tick lf_ticks[LF_MAX_CB];
extern int lf_nticks;
extern struct lf_render lf_renders[LF_MAX_CB];
extern int lf_nrenders;
extern struct lf_hotkey lf_hotkeys[LF_MAX_CB];
extern int lf_nhotkeys;
extern signal_handler_t *lf_handlers[4];
extern obs_frontend_event_cb lf_fe_event_cb[LF_MAX_CB];
extern void *lf_fe_event_p[LF_MAX_CB];
extern int lf_fe_nevent;
extern obs_frontend_save_cb lf_fe_save_cb[LF_MAX_CB];
extern void *lf_fe_save_p[LF_MAX_CB];
extern int lf_fe_nsave;
extern int lf_fe_duration;
extern char lf_fe_scene[32], lf_fe_collection[32], lf_fe_profile[32];

void lf_reset_fakes(void);
bool lf_hide_type(const char *name, int hide);
