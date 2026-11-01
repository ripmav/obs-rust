/* S-056 pipewire conformance runner: shared declarations (test tooling, not part of OBS). */
#pragma once
#include <stdbool.h>
#include "obs.h"
#include "script.h"

/* One fake obs_source_t / plugin instance created from a registered obs_source_info. */
struct pwx_inst {
	char name[24];
	const struct obs_source_info *info;
	void *data;
	obs_data_t *settings;
	obs_properties_t *props;
	uint32_t flags;
	bool used;
};

/* Handles one fake-configuration / fake-event command. Returns false if the command is unknown. */
bool pwx_cmd(const struct script_line *sl);
struct pwx_inst *pwx_inst_get(const char *name, bool create);
const struct obs_source_info *pwx_find_info(const char *id);
void pwx_finish_all(void);
