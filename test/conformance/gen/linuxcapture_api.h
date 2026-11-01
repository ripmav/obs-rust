/* Conformance (area linuxcapture, spec S-049): shared declarations between runner, wrappers and fakes. Test tooling. */
#pragma once
#include "harness.h"
#include "obs.h"

/* Each returns 1 if it handled the command. */
int lc_fake_cmd(struct script_line *sl);  /* linuxcapture_stubs.c: configure fakes, trace */
int lc_v4l2_cmd(struct script_line *sl);  /* linuxcapture_w_v4l2.c */
int lc_pulse_cmd(struct script_line *sl); /* linuxcapture_w_pulse.c */
int lc_xshm_cmd(struct script_line *sl);  /* linuxcapture_w_xshm.c */

/* shared state owned by the runner */
extern obs_data_t *lc_settings;      /* settings object handed to sources */
extern obs_properties_t *lc_props;   /* last properties object */
extern int lc_trace;                 /* print fake system-call trace lines */
void lc_trace_printf(const char *fmt, ...);
void lc_dump_props(obs_properties_t *props, int depth);
void lc_dump_settings(obs_data_t *d);

/* faked obs core knobs */
extern char lc_source_name[64];
extern bool lc_async_unbuffered;
