/* Shared declarations between modservice_runner.c, modservice_stubs.c and the generated fake plugin modules. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

void ms_core_init(void);
void ms_core_set(const char *locale, const char *cfg);
void ms_core_null(bool null_core);
struct obs_core *ms_core(void);
void ms_core_modules_set(char **names, int n, const char *bin_tpl, const char *data_tpl);
