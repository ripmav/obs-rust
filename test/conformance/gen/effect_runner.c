/*
 * Conformance runner for the effect language (spec S-020): effect parser (effect-parser.c),
 * shader parser (shader-parser.c), effect runtime (effect.c) and the effect entry points of
 * graphics.c, running on the fake backend in effect_stubs.c.
 * The command list is documented in test/conformance/README.md ("effect runner").
 *
 * Test tooling, not part of libobs.
 */
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <sys/wait.h>
#include <unistd.h>

#include "graphics/effect.h"
#include "graphics/effect-parser.h"
#include "graphics/shader-parser.h"
#include "graphics/graphics.h"
#include "graphics/vec2.h"
#include "graphics/vec3.h"
#include "graphics/vec4.h"
#include "graphics/matrix4.h"
#include "util/platform.h"
#include "harness.h"

/* from effect_stubs.c */
extern char fake_tmpdir[400];
extern int fake_text_mode;
extern bool fake_parse_shaders;
extern char *fake_fail_msg[2];
extern char *fake_warn_msg[2];
extern const char *fake_preprocessor;
extern int fake_verbose_events;
void *fake_handle_new(int id);
int fake_handle_id(const void *h);
void fake_handles_free(void);
const char *fake_param_type_name(int t);

/* ---------------------------------------------------------------- state */

#define MAX_NAMES 64
static char *text_names[MAX_NAMES];
static char *text_vals[MAX_NAMES];
static int ntexts;

static char *eff_names[MAX_NAMES];
static gs_effect_t *eff_vals[MAX_NAMES];
static int neffs;

static graphics_t *gfx;
static bool ep_brief; /* "backend ep brief": omit function bodies in the ep dump */
static char tmpdir[400];
static char repo_root[1024];

static void die(const char *msg)
{
	fprintf(stderr, "runner: %s\n", msg);
	exit(2);
}

static void find_repo_root(void)
{
	const char *env = getenv("OBS_REPO_ROOT");
	if (env) {
		snprintf(repo_root, sizeof repo_root, "%s", env);
		return;
	}
	const char *marker = "/test/conformance/gen/effect_runner.c";
	const char *f = __FILE__;
	const char *at = strstr(f, marker);
	if (at && at != f) {
		snprintf(repo_root, sizeof repo_root, "%.*s", (int)(at - f), f);
		return;
	}
	snprintf(repo_root, sizeof repo_root, "../..");
}

static void ensure_gfx(void)
{
	if (gfx)
		return;
	if (gs_create(&gfx, "fake", 0) != GS_SUCCESS)
		die("gs_create failed");
	gs_enter_context(gfx);
}

static void free_effects(void)
{
	for (int i = 0; i < neffs; i++) {
		/* uncached effects (no file name) are owned by the caller; cached ones are freed by gs_destroy */
		gs_effect_destroy(eff_vals[i]);
		free(eff_names[i]);
		eff_names[i] = NULL;
		eff_vals[i] = NULL;
	}
	neffs = 0;
}

static void reset_gfx(void)
{
	free_effects();
	if (gfx) {
		gs_leave_context();
		gs_destroy(gfx);
		gfx = NULL;
	}
	fake_handles_free();
}

static const char *text_get(const char *name)
{
	for (int i = 0; i < ntexts; i++)
		if (!strcmp(text_names[i], name))
			return text_vals[i];
	fprintf(stderr, "runner: unknown text block '%s'\n", name);
	exit(2);
}

static void text_set(const char *name, const char *val)
{
	for (int i = 0; i < ntexts; i++)
		if (!strcmp(text_names[i], name)) {
			free(text_vals[i]);
			text_vals[i] = strdup(val);
			return;
		}
	if (ntexts >= MAX_NAMES)
		die("too many text blocks");
	text_names[ntexts] = strdup(name);
	text_vals[ntexts++] = strdup(val);
}

static char *read_repo_file(const char *rel)
{
	char path[2048];
	snprintf(path, sizeof path, "%s/%s", repo_root, rel);
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "runner: cannot open %s\n", rel);
		exit(2);
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = malloc((size_t)n + 1);
	size_t got = fread(buf, 1, (size_t)n, f);
	buf[got] = 0;
	fclose(f);
	return buf;
}

/* Resolves a source argument: @text:NAME, @file:REPO_RELATIVE_PATH or a literal. Returns malloc'd. */
static char *get_src(const char *a)
{
	if (!strncmp(a, "@text:", 6))
		return strdup(text_get(a + 6));
	if (!strncmp(a, "@file:", 6))
		return read_repo_file(a + 6);
	return strdup(a);
}

static const char *nul(const char *a)
{
	return strcmp(a, "@null") ? a : NULL;
}

static gs_effect_t *eff_find(const char *name)
{
	if (!strcmp(name, "@null"))
		return NULL;
	for (int i = 0; i < neffs; i++)
		if (!strcmp(eff_names[i], name))
			return eff_vals[i];
	fprintf(stderr, "runner: unknown effect '%s'\n", name);
	exit(2);
}

static void eff_put(const char *name, gs_effect_t *e)
{
	for (int i = 0; i < neffs; i++)
		if (!strcmp(eff_names[i], name)) {
			eff_vals[i] = e;
			return;
		}
	if (neffs >= MAX_NAMES)
		die("too many effects");
	eff_names[neffs] = strdup(name);
	eff_vals[neffs++] = e;
}

/* ---------------------------------------------------------------- printing */

#define pq script_print_quoted

static void print_hex(const void *p, size_t n)
{
	if (!n) {
		putchar('-');
		return;
	}
	for (size_t i = 0; i < n; i++)
		printf("%02x", ((const uint8_t *)p)[i]);
}

static const char *ptype(enum gs_shader_param_type t)
{
	return fake_param_type_name((int)t);
}

static const char *var_type_name(int v)
{
	switch (v) {
	case 0:
		return "IN";
	case 1:
		return "INOUT";
	case 2:
		return "OUT";
	case 3:
		return "UNIFORM";
	case 4:
		return "CONST";
	}
	return "?";
}

static const char *level_name(int l)
{
	return l == LEX_ERROR ? "error" : l == LEX_WARNING ? "warning" : "?";
}

static void print_errors(struct error_data *ed, const char *indent)
{
	printf("%serrors count=%zu has_errors=%d\n", indent, ed->errors.num, error_data_has_errors(ed) ? 1 : 0);
	for (size_t i = 0; i < ed->errors.num; i++) {
		struct error_item *it = ed->errors.array + i;
		printf("%s  err %zu %s file=", indent, i, level_name(it->level));
		pq(it->file ? it->file : "(null)");
		printf(" row=%u col=%u msg=", it->row, it->column);
		pq(it->error);
		putchar('\n');
	}
}

/* default value rendering: raw bytes plus a decoded view for numeric types */
static void print_default(enum gs_shader_param_type t, const uint8_t *v, size_t n)
{
	printf("default_size=%zu default=", n);
	if ((t == GS_SHADER_PARAM_STRING || t == GS_SHADER_PARAM_TEXTURE) && n) {
		pq((const char *)v);
		return;
	}
	print_hex(v, n);
	if (!n)
		return;
	if (t == GS_SHADER_PARAM_FLOAT || t == GS_SHADER_PARAM_VEC2 || t == GS_SHADER_PARAM_VEC3 ||
	    t == GS_SHADER_PARAM_VEC4 || t == GS_SHADER_PARAM_MATRIX4X4 || t == GS_SHADER_PARAM_UNKNOWN) {
		printf(" f=[");
		for (size_t i = 0; i + 4 <= n; i += 4) {
			float f;
			memcpy(&f, v + i, 4);
			printf("%s%.9g", i ? "," : "", f);
		}
		putchar(']');
	} else if (t == GS_SHADER_PARAM_INT || t == GS_SHADER_PARAM_INT2 || t == GS_SHADER_PARAM_INT3 ||
		   t == GS_SHADER_PARAM_INT4 || t == GS_SHADER_PARAM_BOOL) {
		printf(" l=[");
		for (size_t i = 0; i + sizeof(long) <= n; i += sizeof(long)) {
			long l;
			memcpy(&l, v + i, sizeof l);
			printf("%s%ld", i ? "," : "", l);
		}
		putchar(']');
	}
}

static void print_eparam(const char *ind, size_t i, const gs_eparam_t *p)
{
	printf("%sparam %zu name=%s type=%s annotations=%zu ", ind, i, p->name, ptype(p->type), p->annotations.num);
	print_default(p->type, p->default_val.array, p->default_val.num);
	putchar('\n');
	for (size_t j = 0; j < p->annotations.num; j++) {
		const struct gs_effect_param *a = p->annotations.array + j;
		printf("%s  annotation %zu name=%s type=%s ", ind, j, a->name, ptype(a->type));
		print_default(a->type, a->default_val.array, a->default_val.num);
		putchar('\n');
	}
}

static void dump_effect(const char *name, gs_effect_t *e)
{
	if (!e) {
		printf("dump %s null\n", name);
		return;
	}
	printf("effect %s params=%zu techniques=%zu viewproj=%s world=%s\n", name, e->params.num, e->techniques.num,
	       e->view_proj ? e->view_proj->name : "-", e->world ? e->world->name : "-");
	for (size_t i = 0; i < e->params.num; i++)
		print_eparam("  ", i, e->params.array + i);
	for (size_t i = 0; i < e->techniques.num; i++) {
		struct gs_effect_technique *t = e->techniques.array + i;
		printf("  technique %zu name=%s passes=%zu\n", i, t->name, t->passes.num);
		for (size_t j = 0; j < t->passes.num; j++) {
			struct gs_effect_pass *p = t->passes.array + j;
			printf("    pass %zu name=", j);
			if (p->name)
				pq(p->name);
			else
				printf("@null");
			printf(" vertex_params=[");
			for (size_t k = 0; k < p->vertshader_params.num; k++)
				printf("%s%s", k ? "," : "", p->vertshader_params.array[k].eparam->name);
			printf("] pixel_params=[");
			for (size_t k = 0; k < p->pixelshader_params.num; k++)
				printf("%s%s", k ? "," : "", p->pixelshader_params.array[k].eparam->name);
			printf("] vertex=%s pixel=%s\n", p->vertshader ? "yes" : "no", p->pixelshader ? "yes" : "no");
		}
	}
}

/* ---------------------------------------------------------------- parser internals dump */

static void print_token_array(const cf_token_array_t *a)
{
	putchar('[');
	bool first = true;
	if (a->array)
		for (const struct cf_token *t = a->array; t < a->array + a->num && t->type != CFTOKEN_NONE; t++) {
			char buf[1024];
			size_t n = t->str.len < sizeof(buf) - 1 ? t->str.len : sizeof(buf) - 1;
			memcpy(buf, t->str.array, n);
			buf[n] = 0;
			if (!first)
				putchar(',');
			first = false;
			pq(buf);
		}
	putchar(']');
}

static void print_strlist(char *const *a, size_t n)
{
	putchar('[');
	for (size_t i = 0; i < n; i++) {
		if (i)
			putchar(',');
		pq(a[i]);
	}
	putchar(']');
}

static void print_epvar(const char *ind, const struct ep_var *v)
{
	printf("%svar type=%s name=%s mapping=", ind, v->type, v->name);
	if (v->mapping)
		pq(v->mapping);
	else
		printf("@null");
	printf(" var_type=%s\n", var_type_name((int)v->var_type));
}

static void dump_ep(struct effect_parser *ep)
{
	printf("  ep params=%zu structs=%zu funcs=%zu samplers=%zu techniques=%zu\n", ep->params.num, ep->structs.num,
	       ep->funcs.num, ep->samplers.num, ep->techniques.num);
	for (size_t i = 0; i < ep->params.num; i++) {
		struct ep_param *p = ep->params.array + i;
		printf("  ep_param %zu type=%s name=%s property=%d const=%d uniform=%d texture=%d array_count=%d "
		       "annotations=%zu",
		       i, p->type, p->name, p->is_property, p->is_const, p->is_uniform, p->is_texture, p->array_count,
		       p->annotations.num);
		putchar('\n');
		for (size_t j = 0; j < p->annotations.num; j++) {
			struct ep_param *a = p->annotations.array + j;
			printf("    ep_annotation %zu type=%s name=%s", j, a->type, a->name);
			putchar('\n');
		}
	}
	for (size_t i = 0; i < ep->structs.num; i++) {
		struct ep_struct *s = ep->structs.array + i;
		printf("  ep_struct %zu name=%s vars=%zu\n", i, s->name, s->vars.num);
		for (size_t j = 0; j < s->vars.num; j++)
			print_epvar("    ", s->vars.array + j);
	}
	for (size_t i = 0; i < ep->funcs.num; i++) {
		struct ep_func *f = ep->funcs.array + i;
		printf("  ep_func %zu name=%s ret=%s mapping=", i, f->name, f->ret_type);
		if (f->mapping)
			pq(f->mapping);
		else
			printf("@null");
		printf(" params=%zu\n", f->param_vars.num);
		for (size_t j = 0; j < f->param_vars.num; j++)
			print_epvar("    ", f->param_vars.array + j);
		printf("    struct_deps=");
		print_strlist(f->struct_deps.array, f->struct_deps.num);
		printf(" func_deps=");
		print_strlist(f->func_deps.array, f->func_deps.num);
		printf(" param_deps=");
		print_strlist(f->param_deps.array, f->param_deps.num);
		printf(" sampler_deps=");
		print_strlist(f->sampler_deps.array, f->sampler_deps.num);
		if (ep_brief) {
			printf(" contents_len=%zu\n", f->contents.len);
		} else {
			printf("\n    contents=");
			pq(f->contents.array ? f->contents.array : "");
			putchar('\n');
		}
	}
	for (size_t i = 0; i < ep->samplers.num; i++) {
		struct ep_sampler *s = ep->samplers.array + i;
		printf("  ep_sampler %zu name=%s states=%zu\n", i, s->name, s->states.num);
		for (size_t j = 0; j < s->states.num; j++) {
			printf("    state ");
			pq(s->states.array[j]);
			printf(" = ");
			pq(s->values.array[j]);
			putchar('\n');
		}
	}
	for (size_t i = 0; i < ep->techniques.num; i++) {
		struct ep_technique *t = ep->techniques.array + i;
		printf("  ep_technique %zu name=%s passes=%zu\n", i, t->name, t->passes.num);
		for (size_t j = 0; j < t->passes.num; j++) {
			struct ep_pass *p = t->passes.array + j;
			printf("    ep_pass %zu name=", j);
			if (p->name)
				pq(p->name);
			else
				printf("@null");
			printf("\n      vertex=");
			print_token_array(&p->vertex_program);
			printf("\n      fragment=");
			print_token_array(&p->fragment_program);
			putchar('\n');
		}
	}
}

static void cmd_ep(const char *label, const char *src)
{
	ensure_gfx();
	gs_effect_t *effect = bzalloc(sizeof(struct gs_effect));
	struct effect_parser ep;
	effect->graphics = gfx;
	effect->effect_path = bstrdup(label);
	ep_init(&ep);
	bool ok = ep_parse(&ep, effect, src, label);
	printf("ep result=%s\n", ok ? "ok" : "fail");
	print_errors(&ep.cfp.error_list, "  ");
	dump_ep(&ep);
	char *errs = error_data_buildstring(&ep.cfp.error_list);
	printf("  error_string=");
	if (errs)
		pq(errs);
	else
		printf("@null");
	putchar('\n');
	bfree(errs);
	if (ok)
		dump_effect("(effect)", effect);
	/* an effect that failed or has no path is owned by the caller; same as gs_effect_create */
	effect_free(effect);
	bfree(effect);
	ep_free(&ep);
}

/* ---------------------------------------------------------------- shader parser dump */

static void print_svar(const char *ind, const char *kind, size_t i, const struct shader_var *v)
{
	printf("%s%s %zu type=%s name=%s mapping=", ind, kind, i, v->type, v->name);
	if (v->mapping)
		pq(v->mapping);
	else
		printf("@null");
	printf(" var_type=%s array_count=%d ", var_type_name((int)v->var_type), v->array_count);
	print_default(get_shader_param_type(v->type), v->default_val.array, v->default_val.num);
	putchar('\n');
}

static const char *filter_name(int f)
{
	static const char *n[] = {"POINT",
				  "LINEAR",
				  "ANISOTROPIC",
				  "MIN_MAG_POINT_MIP_LINEAR",
				  "MIN_POINT_MAG_LINEAR_MIP_POINT",
				  "MIN_POINT_MAG_MIP_LINEAR",
				  "MIN_LINEAR_MAG_MIP_POINT",
				  "MIN_LINEAR_MAG_POINT_MIP_LINEAR",
				  "MIN_MAG_LINEAR_MIP_POINT"};
	return f >= 0 && f < 9 ? n[f] : "?";
}

static const char *addr_name(int a)
{
	static const char *n[] = {"CLAMP", "WRAP", "MIRROR", "BORDER", "MIRRORONCE"};
	return a >= 0 && a < 5 ? n[a] : "?";
}

static void cmd_sparse(const char *label, const char *src)
{
	struct shader_parser sp;
	shader_parser_init(&sp);
	bool ok = shader_parse(&sp, src, label);
	printf("sparse result=%s\n", ok ? "ok" : "fail");
	print_errors(&sp.cfp.error_list, "  ");
	char *errs = shader_parser_geterrors(&sp);
	printf("  error_string=");
	if (errs)
		pq(errs);
	else
		printf("@null");
	putchar('\n');
	bfree(errs);
	printf("  sp params=%zu structs=%zu samplers=%zu funcs=%zu\n", sp.params.num, sp.structs.num, sp.samplers.num,
	       sp.funcs.num);
	for (size_t i = 0; i < sp.params.num; i++)
		print_svar("  ", "param", i, sp.params.array + i);
	for (size_t i = 0; i < sp.structs.num; i++) {
		struct shader_struct *s = sp.structs.array + i;
		printf("  struct %zu name=%s vars=%zu\n", i, s->name, s->vars.num);
		for (size_t j = 0; j < s->vars.num; j++)
			print_svar("    ", "var", j, s->vars.array + j);
	}
	for (size_t i = 0; i < sp.samplers.num; i++) {
		struct shader_sampler *s = sp.samplers.array + i;
		struct gs_sampler_info info;
		printf("  sampler %zu name=%s states=%zu\n", i, s->name, s->states.num);
		for (size_t j = 0; j < s->states.num; j++) {
			printf("    state ");
			pq(s->states.array[j]);
			printf(" = ");
			pq(s->values.array[j]);
			putchar('\n');
		}
		shader_sampler_convert(s, &info);
		printf("    info filter=%s address_u=%s address_v=%s address_w=%s max_anisotropy=%d border_color=%08x\n",
		       filter_name(info.filter), addr_name(info.address_u), addr_name(info.address_v),
		       addr_name(info.address_w), info.max_anisotropy, (unsigned)info.border_color);
	}
	for (size_t i = 0; i < sp.funcs.num; i++) {
		struct shader_func *f = sp.funcs.array + i;
		printf("  func %zu name=%s return_type=%s mapping=", i, f->name, f->return_type);
		if (f->mapping)
			pq(f->mapping);
		else
			printf("@null");
		printf(" params=%zu\n", f->params.num);
		for (size_t j = 0; j < f->params.num; j++)
			print_svar("    ", "param", j, f->params.array + j);
		struct dstr body;
		dstr_init(&body);
		size_t ntok = 0;
		for (struct cf_token *t = f->start; t && t != f->end && t->type != CFTOKEN_NONE; t++, ntok++)
			dstr_ncat(&body, t->str.array, t->str.len);
		printf("    body_tokens=%zu body=", ntok);
		pq(body.array ? body.array : "");
		putchar('\n');
		dstr_free(&body);
	}
	shader_parser_free(&sp);
}

/* ---------------------------------------------------------------- handles */

struct pref {
	gs_effect_t *effect;
	gs_eparam_t *param;
	bool is_null;
};

/* "@null" | "E:#IDX" | "E:NAME" | "E:#IDX/#AIDX" | "E:NAME/ANNOT" */
static gs_eparam_t *resolve_param(const char *spec, gs_effect_t **effp)
{
	if (!strcmp(spec, "@null")) {
		if (effp)
			*effp = NULL;
		return NULL;
	}
	char buf[512];
	snprintf(buf, sizeof buf, "%s", spec);
	char *colon = strchr(buf, ':');
	if (!colon)
		die("param spec needs E:PARAM");
	*colon = 0;
	gs_effect_t *e = eff_find(buf);
	if (effp)
		*effp = e;
	char *sel = colon + 1;
	char *slash = strchr(sel, '/');
	if (slash)
		*slash = 0;
	gs_eparam_t *p = sel[0] == '#' ? gs_effect_get_param_by_idx(e, (size_t)strtoull(sel + 1, NULL, 0))
				       : gs_effect_get_param_by_name(e, sel);
	if (slash) {
		const char *a = slash + 1;
		p = a[0] == '#' ? gs_param_get_annotation_by_idx(p, (size_t)strtoull(a + 1, NULL, 0))
				: gs_param_get_annotation_by_name(p, a);
	}
	return p;
}

static gs_technique_t *get_tech(const char *e, const char *t)
{
	return gs_effect_get_technique(eff_find(e), t);
}

static void print_param_name(const gs_eparam_t *p)
{
	if (!p) {
		printf("null");
		return;
	}
	struct gs_effect_param_info info;
	gs_effect_get_param_info(p, &info);
	printf("name=%s type=%s", info.name, ptype(info.type));
}

/* ---------------------------------------------------------------- commands */

static int run_command(struct script_line *sl);

static void do_effect(struct script_line *sl, bool want_err)
{
	script_need(sl, 4);
	ensure_gfx();
	char *src = get_src(sl->argv[3]);
	char *err = NULL;
	gs_effect_t *e = gs_effect_create(src, nul(sl->argv[2]), want_err ? &err : NULL);
	printf("effect %s result=%s\n", sl->argv[1], e ? "ok" : "fail");
	if (want_err) {
		printf("  error_string=");
		if (err)
			pq(err);
		else
			printf("@null");
		putchar('\n');
	}
	bfree(err);
	eff_put(sl->argv[1], e);
	free(src);
}

static int run_command(struct script_line *sl)
{
	const char *c = sl->argv[0];

#define IS(x) (!strcmp(c, (x)))

	if (IS("gfx")) {
		/* gfx [PREPROCESSOR|@null]: (re)create the fake graphics context */
		reset_gfx();
		free((void *)fake_preprocessor);
		fake_preprocessor = sl->argc > 1 && nul(sl->argv[1]) ? strdup(sl->argv[1]) : NULL;
		ensure_gfx();
		printf("gfx preprocessor=%s\n", fake_preprocessor ? fake_preprocessor : "@null");
	} else if (IS("reset")) {
		reset_gfx();
		printf("reset\n");
	} else if (IS("backend")) {
		script_need(sl, 3);
		const char *k = sl->argv[1];
		if (!strcmp(k, "text"))
			fake_text_mode = !strcmp(sl->argv[2], "full") ? 2 : !strcmp(sl->argv[2], "hash") ? 1 : 0;
		else if (!strcmp(k, "parse"))
			fake_parse_shaders = !strcmp(sl->argv[2], "on");
		else if (!strcmp(k, "ep"))
			ep_brief = !strcmp(sl->argv[2], "brief");
		else if (!strcmp(k, "events"))
			fake_verbose_events = !strcmp(sl->argv[2], "on");
		else if (!strcmp(k, "fail") || !strcmp(k, "warn")) {
			script_need(sl, 4);
			char **arr = !strcmp(k, "fail") ? fake_fail_msg : fake_warn_msg;
			int i = !strcmp(sl->argv[2], "pixel");
			free(arr[i]);
			arr[i] = strcmp(sl->argv[3], "off") ? strdup(sl->argv[3]) : NULL;
		} else
			die("bad backend key");
	} else if (IS("text")) {
		/* handled in main (block syntax) */
		die("text block handled by main loop");
	} else if (IS("textf")) {
		script_need(sl, 3);
		char *t = read_repo_file(sl->argv[2]);
		text_set(sl->argv[1], t);
		free(t);
	} else if (IS("effect")) {
		do_effect(sl, true);
	} else if (IS("effect_noerr")) {
		do_effect(sl, false);
	} else if (IS("effect_from_file")) {
		/* effect_from_file NAME @tmp/FILE: gs_effect_create_from_file (cache lookup by path) */
		script_need(sl, 3);
		ensure_gfx();
		char path[1024];
		snprintf(path, sizeof path, "%s%s", tmpdir, sl->argv[2] + (strncmp(sl->argv[2], "@tmp", 4) ? 0 : 4));
		char *err = NULL;
		gs_effect_t *e = gs_effect_create_from_file(path, &err);
		int same = -1;
		for (int i = 0; i < neffs; i++)
			if (eff_vals[i] == e && e)
				same = i;
		printf("effect_from_file %s result=%s same_as=%s\n", sl->argv[1], e ? "ok" : "fail",
		       same >= 0 ? eff_names[same] : "-");
		bfree(err);
		eff_put(sl->argv[1], e);
	} else if (IS("writefile")) {
		script_need(sl, 3);
		char path[1024];
		snprintf(path, sizeof path, "%s/%s", tmpdir, sl->argv[1]);
		char *t = get_src(sl->argv[2]);
		FILE *f = fopen(path, "wb");
		if (!f)
			die("writefile");
		fwrite(t, 1, strlen(t), f);
		fclose(f);
		free(t);
	} else if (IS("mkdir")) {
		script_need(sl, 2);
		char path[1024];
		snprintf(path, sizeof path, "%s/%s", tmpdir, sl->argv[1]);
		if (mkdir(path, 0755) != 0 && errno != EEXIST)
			die("mkdir");
	} else if (IS("cd")) {
		/* cd @tmp | cd @repo: the working directory decides where "#include" paths are resolved */
		script_need(sl, 2);
		const char *d = !strcmp(sl->argv[1], "@tmp") ? tmpdir : repo_root;
		if (chdir(d) != 0)
			die("cd");
	} else if (IS("ep")) {
		script_need(sl, 3);
		char *src = get_src(sl->argv[2]);
		cmd_ep(nul(sl->argv[1]), src);
		free(src);
	} else if (IS("sparse")) {
		script_need(sl, 3);
		char *src = get_src(sl->argv[2]);
		cmd_sparse(nul(sl->argv[1]), src);
		free(src);
	} else if (IS("dump")) {
		script_need(sl, 2);
		dump_effect(sl->argv[1], eff_find(sl->argv[1]));
	} else if (IS("destroy")) {
		script_need(sl, 2);
		gs_effect_t *e = eff_find(sl->argv[1]);
		bool cached = e && e->cached;
		gs_effect_destroy(e);
		printf("destroy %s cached=%d\n", sl->argv[1], cached ? 1 : 0);
		if (e && !cached)
			eff_put(sl->argv[1], NULL);
	} else if (IS("ptype")) {
		script_need(sl, 2);
		printf("ptype %s -> %s\n", sl->argv[1], ptype(get_shader_param_type(sl->argv[1])));
	} else if (IS("filter")) {
		script_need(sl, 2);
		printf("filter %s -> %s\n", sl->argv[1], filter_name(get_sample_filter(sl->argv[1])));
	} else if (IS("addr")) {
		script_need(sl, 2);
		printf("addr %s -> %s\n", sl->argv[1], addr_name(get_address_mode(sl->argv[1])));
	}
	/* ---- runtime: techniques and passes ---- */
	else if (IS("technique")) {
		script_need(sl, 3);
		gs_technique_t *t = get_tech(sl->argv[1], sl->argv[2]);
		printf("technique %s -> ", sl->argv[2]);
		if (t)
			printf("found passes=%zu\n", t->passes.num);
		else
			printf("null\n");
	} else if (IS("current_technique")) {
		script_need(sl, 2);
		gs_technique_t *t = gs_effect_get_current_technique(eff_find(sl->argv[1]));
		printf("current_technique -> %s\n", t ? t->name : "null");
	} else if (IS("tbegin")) {
		script_need(sl, 3);
		printf("tbegin -> %zu\n", gs_technique_begin(get_tech(sl->argv[1], sl->argv[2])));
	} else if (IS("tend")) {
		script_need(sl, 3);
		gs_technique_end(get_tech(sl->argv[1], sl->argv[2]));
		printf("tend\n");
	} else if (IS("pbegin")) {
		script_need(sl, 4);
		bool r = gs_technique_begin_pass(get_tech(sl->argv[1], sl->argv[2]),
						 (size_t)strtoull(sl->argv[3], NULL, 0));
		printf("pbegin -> %d\n", r);
	} else if (IS("pbegin_name")) {
		script_need(sl, 4);
		bool r = gs_technique_begin_pass_by_name(get_tech(sl->argv[1], sl->argv[2]), sl->argv[3]);
		printf("pbegin_name -> %d\n", r);
	} else if (IS("pend")) {
		script_need(sl, 3);
		gs_technique_end_pass(get_tech(sl->argv[1], sl->argv[2]));
		printf("pend\n");
	} else if (IS("update")) {
		script_need(sl, 2);
		gs_effect_update_params(eff_find(sl->argv[1]));
		printf("update\n");
	} else if (IS("loop")) {
		/* one call of gs_effect_loop */
		script_need(sl, 3);
		bool r = gs_effect_loop(eff_find(sl->argv[1]), nul(sl->argv[2]));
		printf("loop -> %d\n", r);
	} else if (IS("loop_all")) {
		/* loop_all E TECH [MAX]: while (gs_effect_loop) { count } */
		script_need(sl, 3);
		int max = sl->argc > 3 ? atoi(sl->argv[3]) : 100;
		int n = 0;
		gs_effect_t *e = eff_find(sl->argv[1]);
		while (n < max && gs_effect_loop(e, sl->argv[2]))
			n++;
		printf("loop_all iterations=%d\n", n);
	} else if (IS("pass_by_idx")) {
		script_need(sl, 4);
		gs_technique_t *t = get_tech(sl->argv[1], sl->argv[2]);
		size_t idx = (size_t)strtoull(sl->argv[3], NULL, 0);
		gs_epass_t *p = gs_technique_get_pass_by_idx(t, idx);
		printf("pass_by_idx %zu -> ", idx);
		if (!p)
			printf("null\n");
		else if (idx >= t->passes.num)
			printf("past-end\n");
		else if (p->name)
			printf("name=%s\n", p->name);
		else
			printf("name=@null\n");
	} else if (IS("pass_by_name")) {
		script_need(sl, 4);
		gs_technique_t *t = get_tech(sl->argv[1], sl->argv[2]);
		gs_epass_t *p = gs_technique_get_pass_by_name(t, sl->argv[3]);
		printf("pass_by_name %s -> ", sl->argv[3]);
		if (!p)
			printf("null\n");
		else
			printf("index=%d\n", (int)(p - t->passes.array));
	}
	/* ---- runtime: parameters ---- */
	else if (IS("nparams")) {
		script_need(sl, 2);
		printf("nparams -> %zu\n", gs_effect_get_num_params(eff_find(sl->argv[1])));
	} else if (IS("param")) {
		/* param E:#I|E:NAME[/ANNOT] */
		script_need(sl, 2);
		gs_eparam_t *p = resolve_param(sl->argv[1], NULL);
		printf("param %s -> ", sl->argv[1]);
		print_param_name(p);
		putchar('\n');
	} else if (IS("param_null_name")) {
		/* gs_effect_get_param_by_name(effect, NULL): DEFECT when the effect has parameters */
		script_need(sl, 2);
		gs_eparam_t *p = gs_effect_get_param_by_name(eff_find(sl->argv[1]), NULL);
		printf("param_null_name -> %s\n", p ? "non-null" : "null");
	} else if (IS("annotation_null_name")) {
		script_need(sl, 2);
		gs_eparam_t *p = gs_param_get_annotation_by_name(resolve_param(sl->argv[1], NULL), NULL);
		printf("annotation_null_name -> %s\n", p ? "non-null" : "null");
	} else if (IS("nannotations")) {
		script_need(sl, 2);
		printf("nannotations -> %zu\n", gs_param_get_num_annotations(resolve_param(sl->argv[1], NULL)));
	} else if (IS("annotation_idx")) {
		/* annotation_idx E:PARAM IDX: raw gs_param_get_annotation_by_idx (note the bound check quirk) */
		script_need(sl, 3);
		gs_eparam_t *p = resolve_param(sl->argv[1], NULL);
		size_t idx = (size_t)strtoull(sl->argv[2], NULL, 0);
		gs_eparam_t *a = gs_param_get_annotation_by_idx(p, idx);
		printf("annotation_idx %zu -> ", idx);
		if (!a)
			printf("null\n");
		else if (idx >= p->annotations.num)
			printf("past-end\n");
		else {
			print_param_name(a);
			putchar('\n');
		}
	} else if (IS("viewproj") || IS("world")) {
		script_need(sl, 2);
		gs_eparam_t *p = IS("viewproj") ? gs_effect_get_viewproj_matrix(eff_find(sl->argv[1]))
						: gs_effect_get_world_matrix(eff_find(sl->argv[1]));
		printf("%s -> ", c);
		print_param_name(p);
		putchar('\n');
	} else if (IS("set_bool")) {
		script_need(sl, 3);
		gs_effect_set_bool(resolve_param(sl->argv[1], NULL), atoi(sl->argv[2]) != 0);
	} else if (IS("set_float")) {
		script_need(sl, 3);
		gs_effect_set_float(resolve_param(sl->argv[1], NULL), strtof(sl->argv[2], NULL));
	} else if (IS("set_int")) {
		script_need(sl, 3);
		gs_effect_set_int(resolve_param(sl->argv[1], NULL), (int)strtol(sl->argv[2], NULL, 0));
	} else if (IS("set_vec2")) {
		script_need(sl, 4);
		struct vec2 v;
		vec2_set(&v, strtof(sl->argv[2], NULL), strtof(sl->argv[3], NULL));
		gs_effect_set_vec2(resolve_param(sl->argv[1], NULL), &v);
	} else if (IS("set_vec3")) {
		script_need(sl, 5);
		struct vec3 v;
		vec3_set(&v, strtof(sl->argv[2], NULL), strtof(sl->argv[3], NULL), strtof(sl->argv[4], NULL));
		gs_effect_set_vec3(resolve_param(sl->argv[1], NULL), &v);
	} else if (IS("set_vec4")) {
		script_need(sl, 6);
		struct vec4 v;
		vec4_set(&v, strtof(sl->argv[2], NULL), strtof(sl->argv[3], NULL), strtof(sl->argv[4], NULL),
			 strtof(sl->argv[5], NULL));
		gs_effect_set_vec4(resolve_param(sl->argv[1], NULL), &v);
	} else if (IS("set_color")) {
		script_need(sl, 3);
		gs_effect_set_color(resolve_param(sl->argv[1], NULL), (uint32_t)strtoull(sl->argv[2], NULL, 0));
	} else if (IS("set_matrix4")) {
		script_need(sl, 18);
		struct matrix4 m;
		float *f = (float *)&m;
		for (int i = 0; i < 16; i++)
			f[i] = strtof(sl->argv[2 + i], NULL);
		gs_effect_set_matrix4(resolve_param(sl->argv[1], NULL), &m);
	} else if (IS("set_matrix4_null")) {
		script_need(sl, 2);
		gs_effect_set_matrix4(resolve_param(sl->argv[1], NULL), NULL);
	} else if (IS("set_val")) {
		/* set_val E:PARAM HEX|- : gs_effect_set_val with the decoded bytes (NULL data with @null) */
		script_need(sl, 3);
		size_t n;
		uint8_t *b = script_parse_hex(sl->argv[2], &n);
		gs_effect_set_val(resolve_param(sl->argv[1], NULL), strcmp(sl->argv[2], "@null") ? b : NULL, n);
		free(b);
	} else if (IS("set_texture") || IS("set_texture_srgb")) {
		/* set_texture E:PARAM ID  (ID -1 = NULL texture) */
		script_need(sl, 3);
		int id = atoi(sl->argv[2]);
		gs_texture_t *t = id >= 0 ? fake_handle_new(id) : NULL;
		if (IS("set_texture"))
			gs_effect_set_texture(resolve_param(sl->argv[1], NULL), t);
		else
			gs_effect_set_texture_srgb(resolve_param(sl->argv[1], NULL), t);
		/* the handle is leaked on purpose: the effect may keep the pointer until the technique ends */
	} else if (IS("set_next_sampler")) {
		script_need(sl, 3);
		int id = atoi(sl->argv[2]);
		gs_effect_set_next_sampler(resolve_param(sl->argv[1], NULL), id >= 0 ? fake_handle_new(id) : NULL);
	} else if (IS("set_default")) {
		script_need(sl, 2);
		gs_effect_set_default(resolve_param(sl->argv[1], NULL));
	} else if (IS("get_val") || IS("get_default_val")) {
		script_need(sl, 2);
		gs_eparam_t *p = resolve_param(sl->argv[1], NULL);
		bool def = IS("get_default_val");
		void *v = def ? gs_effect_get_default_val(p) : gs_effect_get_val(p);
		size_t sz = def ? gs_effect_get_default_val_size(p) : gs_effect_get_val_size(p);
		printf("%s size=%zu ", c, sz);
		if (!v)
			printf("data=null\n");
		else {
			if (p && p->type == GS_SHADER_PARAM_TEXTURE && !def && sz == sizeof(struct gs_shader_texture)) {
				struct gs_shader_texture *t = v;
				printf("texture=%d srgb=%d\n", fake_handle_id(t->tex), t->srgb ? 1 : 0);
			} else {
				printf("hex=");
				print_hex(v, sz);
				putchar('\n');
			}
			bfree(v);
		}
	} else if (IS("flags")) {
		/* flags E:PARAM: internal change/sampler state of a parameter */
		script_need(sl, 2);
		gs_eparam_t *p = resolve_param(sl->argv[1], NULL);
		printf("flags changed=%d cur_size=%zu next_sampler=%d\n", p ? p->changed : -1, p ? p->cur_val.num : 0,
		       p ? (p->next_sampler ? fake_handle_id(p->next_sampler) : -1) : -2);
	} else if (IS("echo")) {
		for (int i = 1; i < sl->argc; i++) {
			if (i > 1)
				putchar(' ');
			fputs(sl->argv[i], stdout);
		}
		putchar('\n');
	} else {
		fprintf(stderr, "script: line %d: unknown command '%s'\n", sl->lineno, c);
		exit(2);
	}
	return 0;
}

/* Log handler: like harness_log_handler, but a scratch directory path inside a message is printed as "@tmp". */
static void effect_log_handler(int level, const char *format, va_list args, void *param)
{
	char msg[4096];
	(void)param;
	vsnprintf(msg, sizeof(msg), format, args);
	for (char *p = msg; *p; p++)
		if (*p == '\n')
			*p = ' ';
	size_t tl = strlen(tmpdir);
	const char *name = level == LOG_ERROR     ? "error"
			   : level == LOG_WARNING ? "warning"
			   : level == LOG_INFO    ? "info"
					       : "debug";
	printf("log %s ", name);
	for (const char *p = msg; *p;) {
		if (tl && !strncmp(p, tmpdir, tl)) {
			fputs("@tmp", stdout);
			p += tl;
		} else {
			putchar(*p++);
		}
	}
	putchar('\n');
}

static void on_alarm(int sig)
{
	(void)sig;
	_exit(124);
}

int main(int argc, char **argv)
{
#ifdef __GLIBC__
	/* Fill every fresh allocation with a fixed pattern so that the reference's reads of uninitialised or
	 * out-of-bounds heap memory (DEFECT cases) behave identically on every run. */
	mallopt(M_PERTURB, 0xA5);
#endif
	FILE *f = harness_open(argc, argv);
	const char *td = getenv("CONFORMANCE_TMPDIR");
	snprintf(tmpdir, sizeof tmpdir, "%s", td ? td : "/tmp");
	base_set_log_handler(effect_log_handler, NULL);
	snprintf(fake_tmpdir, sizeof fake_tmpdir, "%s", tmpdir);
	find_repo_root();
	if (chdir(repo_root) != 0)
		die("cannot enter repository root");

	struct script_line sl;
	int lineno = 0;
	char raw[SCRIPT_MAX_LINE];

	while (fgets(raw, sizeof raw, f)) {
		lineno++;
		sl.lineno = lineno;
		if (!script_tokenize(&sl, raw))
			continue;

		if (!strcmp(sl.argv[0], "text") && sl.argc == 2) {
			/* text NAME ... lines verbatim ... then a line "@end" */
			size_t cap = 4096, len = 0;
			char *buf = malloc(cap);
			buf[0] = 0;
			bool done = false;
			while (fgets(raw, sizeof raw, f)) {
				lineno++;
				if (!strcmp(raw, "@end\n") || !strcmp(raw, "@end")) {
					done = true;
					break;
				}
				size_t n = strlen(raw);
				if (len + n + 1 > cap) {
					cap = (len + n + 1) * 2;
					buf = realloc(buf, cap);
				}
				memcpy(buf + len, raw, n + 1);
				len += n;
			}
			if (!done)
				die("unterminated text block");
			/* drop the final newline so that a block of one line "x" is "x" */
			if (len && buf[len - 1] == '\n')
				buf[--len] = 0;
			text_set(sl.argv[1], buf);
			free(buf);
			continue;
		}

		if (!strcmp(sl.argv[0], "try") || !strcmp(sl.argv[0], "ignore")) {
			/* try CMD...: run in a child, print its output and how it ended.
			 * ignore CMD...: same, but the output and the outcome are NOT recorded (reference behaviour is
			 * undefined or non-deterministic; the spec states the required behaviour). */
			bool quiet = sl.argv[0][0] == 'i';
			script_need(&sl, 2);
			if (!quiet)
				printf("try-begin\n");
			else
				printf("ignore-begin\n");
			fflush(stdout);
			pid_t pid = fork();
			if (pid == 0) {
				struct script_line sub = sl;
				/* re-point argv into the copied storage */
				for (int i = 0; i < sl.argc; i++)
					sub.argv[i] = sub.storage + (sl.argv[i] - sl.storage);
				for (int i = 1; i < sub.argc; i++)
					sub.argv[i - 1] = sub.argv[i];
				sub.argc--;
				signal(SIGALRM, on_alarm);
				alarm(10);
				if (quiet) {
					FILE *nul = freopen("/dev/null", "w", stdout);
					(void)nul;
				}
				run_command(&sub);
				fflush(stdout);
				_exit(0);
			}
			int status = 0;
			waitpid(pid, &status, 0);
			if (quiet)
				printf("ignore-end\n");
			else if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
				printf("try-end ok\n");
			else if (WIFEXITED(status) && WEXITSTATUS(status) == 124)
				printf("try-end hang\n");
			else
				printf("try-end crash\n");
			continue;
		}

		run_command(&sl);
	}
	fclose(f);
	reset_gfx();
	return 0;
}
