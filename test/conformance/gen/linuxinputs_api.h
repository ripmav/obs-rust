/* Shared declarations for the linuxinputs conformance runner (spec S-062). Test tooling. */
#pragma once
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include "obs.h"
#include "script.h"

extern obs_data_t *li_settings;
extern obs_properties_t *li_props;

void li_init(void);
void li_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void li_flush(void);
void li_set_classifier(int (*fn)(pthread_t));
void li_sleep_ms(int ms);
void li_set_now(uint64_t ns);
uint64_t li_get_now(void);
void li_dump_props(obs_properties_t *props, int depth);
const char *li_afmt(enum audio_format f);
const char *li_spk(enum speaker_layout s);

/* audio recorder (fake obs_source_output_audio) */
extern volatile int li_audio_framebytes;
void li_audio_reset(void);
unsigned li_audio_count(void);
bool li_audio_wait(unsigned n, int timeout_ms);
void li_audio_dump(void);

/* per-module command handlers; return true if the command was handled */
bool li_alsa_cmd(struct script_line *sl);
bool li_xcomp_cmd(struct script_line *sl);
bool li_oss_cmd(struct script_line *sl);
bool li_sndio_cmd(struct script_line *sl);
bool li_jack_cmd(struct script_line *sl);
bool li_v4l2out_cmd(struct script_line *sl);
bool li_udev_cmd(struct script_line *sl);
