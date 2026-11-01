/* Conformance harness (area graphics, spec S-043): shared declarations of the tracing fake backend. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/bmem.h"
#include "util/base.h"
#include "util/platform.h"
#include "graphics/graphics-internal.h"

struct fake_sym {
	const char *name;
	void *fn;
};
extern const struct fake_sym fake_symtab[];

/* A backend object: every pointer a fake returns is one of these (so no address ever reaches the output). */
struct fh {
	int id;
	char kind[40];
	uint32_t w, h, d; /* texture / stage surface geometry */
	int fmt;
	int ttype; /* enum gs_texture_type */
	uint8_t *map;
	uint32_t linesize;
	struct gs_vb_data *vbd; /* vertex buffer */
	void *ibd;              /* index buffer data */
	size_t inum;
	int itype;
	uint32_t flags;
};

struct fh *fh_new(const char *kind);
struct fh *fh_lookup(const void *p); /* NULL if p is not a live handle */
int fh_id(const void *p);            /* -1 if not a handle */
void fh_free(void *p);
void fh_reset(void);

/* tracing: one line per backend call, "  dev NAME a=v b=v" */
void tr_begin(const char *name);
void tr_end(void);
void tr_str(const char *nm, const char *v);
void tr_ptr(const char *nm, const void *v);
void tr_bool(const char *nm, bool v);
void tr_dbl(const char *nm, double v);
void tr_u64(const char *nm, unsigned long long v);
void tr_i64(const char *nm, long long v);
void tr_fn(const char *nm, const void *v);
void tr_raw(const char *fmt, ...);
extern int tr_enabled;

/* scripted results */
bool ret_bool(const char *fn);
double ret_dbl(const char *fn);
const char *ret_str(const char *fn);
long long ret_i64(const char *fn);
unsigned long long ret_u64(const char *fn);
void *ret_ptr(const char *fn, const char *kind);

/* state shared with the runner */
extern uint32_t fake_w, fake_h;
extern int fake_dump_full;
void fake_set_ret(const char *fn, const char *val);
void fake_set_fail(const char *fn, bool fail);
void fake_set_fail_nth(const char *fn, int n); /* only the n-th call (1-based) of fn from now on fails */
void fake_set_missing(const char *fn, bool missing);
bool fake_is_missing(const char *fn);
void fake_reset_script(void);
void fake_add_adapter(const char *name);
void fake_add_file(const char *path, const char *content);
const char *fake_get_file(const char *path);
