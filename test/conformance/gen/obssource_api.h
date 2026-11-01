/* Conformance harness (spec S-040): interface between obssource_runner.c and obssource_stubs.c. Test tooling. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "obs.h"

void os_set_clock(uint64_t ns);
int os_drain_tasks(void (*before)(void *param));
int os_pending_tasks(void);
bool fk_kind_add(const char *id, enum obs_source_type type, uint32_t output_flags, uint32_t hooks);
uint32_t fk_hook_from_name(const char *n);
void fk_set_size(obs_source_t *s, uint32_t w, uint32_t h);
void fk_set_drop(obs_source_t *s, int drop);
void fk_child_add(obs_source_t *p, obs_source_t *c);
void fk_child_clear(obs_source_t *p);
void core_reset(void);
