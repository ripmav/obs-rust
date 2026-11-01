/* Shared declarations between scripting_runner.c and scripting_stubs.c (fake language backend). */
#pragma once
#include <stdbool.h>

#define FAKE_MAX_DEFS 16

struct fake_def {
	char path[128];
	bool load_ok;
	char desc[128];
	int nprops; /* < 0: backend returns NULL from get_properties */
};

extern struct fake_def fake_defs[FAKE_MAX_DEFS];
extern int fake_ndefs;
extern int (*fake_deferred_counter)(void);
