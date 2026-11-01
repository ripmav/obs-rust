/*
 * Conformance runner for vec3/vec4/quat/matrix4/axisang (spec S-008).
 *
 * Variables are typed values stored by name:
 *   vec3 N x y z [w]     set fields directly (w defaults to 0)
 *   vec4 N x y z w
 *   quat N x y z w       set fields directly
 *   aa N x y z angle     axis-angle (angle in w)
 *   mat4 N 16 floats     rows x,y,z,t (row-major as listed)
 *   quat_set_fn N x y z w   call quat_set()   (reversed arguments, see spec)
 *   call D fn args...    D = fn(args); args are variable names or float literals
 *   show N               print a variable
 * Floats print with %.9g. 64-bit x86-64 SSE semantic; compare with 1e-6 relative tolerance.
 */
#include <math.h>
#include "graphics/vec3.h"
#include "graphics/vec4.h"
#include "graphics/quat.h"
#include "graphics/matrix4.h"
#include "graphics/axisang.h"
#include "harness.h"

/* defined in matrix4.c but not declared in the public header */
void matrix4_mul_4x3_only(struct matrix4 *dst, const struct matrix4 *m1, const struct matrix4 *m2);

enum { T_NONE, T_V3, T_V4, T_Q, T_M4, T_AA, T_F, T_B };
struct var {
	char name[32];
	int type;
	union {
		struct vec3 v3;
		struct vec4 v4;
		struct quat q;
		struct matrix4 m;
		struct axisang aa;
		float f;
		int b;
	} u;
};
static struct var vars[64];

static struct var *get(const char *n)
{
	for (int i = 0; i < 64; i++)
		if (vars[i].type && !strcmp(vars[i].name, n))
			return &vars[i];
	return NULL;
}
static struct var *make(const char *n, int type)
{
	struct var *v = get(n);
	if (!v)
		for (int i = 0; i < 64; i++)
			if (!vars[i].type) {
				v = &vars[i];
				break;
			}
	/* An existing variable of the same type keeps its contents so that scripts can observe
	 * lanes a function does NOT write. New variables start zeroed. */
	if (v->type != type) {
		memset(v, 0, sizeof *v);
		snprintf(v->name, sizeof v->name, "%s", n);
		v->type = type;
	}
	return v;
}
static void show(const struct var *v)
{
	const float *p;
	switch (v->type) {
	case T_V3: p = v->u.v3.ptr; printf("%s vec3 %.9g %.9g %.9g %.9g\n", v->name, p[0], p[1], p[2], p[3]); break;
	case T_V4: p = v->u.v4.ptr; printf("%s vec4 %.9g %.9g %.9g %.9g\n", v->name, p[0], p[1], p[2], p[3]); break;
	case T_Q: p = v->u.q.ptr; printf("%s quat %.9g %.9g %.9g %.9g\n", v->name, p[0], p[1], p[2], p[3]); break;
	case T_AA: p = v->u.aa.ptr; printf("%s aa %.9g %.9g %.9g %.9g\n", v->name, p[0], p[1], p[2], p[3]); break;
	case T_M4:
		p = (const float *)&v->u.m;
		printf("%s mat4", v->name);
		for (int i = 0; i < 16; i++)
			printf(" %.9g", p[i]);
		printf("\n");
		break;
	case T_F: printf("%s float %.9g\n", v->name, v->u.f); break;
	case T_B: printf("%s bool %d\n", v->name, v->u.b); break;
	}
}
#define A(i) get(sl.argv[i])
#define NUM(i) ((float)strtod(sl.argv[i], NULL))

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;
	while (script_next(f, &sl, &lineno)) {
		const char *c = sl.argv[0];
		if (!strcmp(c, "vec3")) {
			struct var *v = make(sl.argv[1], T_V3);
			v->u.v3.x = NUM(2); v->u.v3.y = NUM(3); v->u.v3.z = NUM(4);
			v->u.v3.w = sl.argc > 5 ? NUM(5) : 0.0f;
		} else if (!strcmp(c, "vec4")) {
			struct var *v = make(sl.argv[1], T_V4);
			vec4_set(&v->u.v4, NUM(2), NUM(3), NUM(4), NUM(5));
		} else if (!strcmp(c, "quat")) {
			struct var *v = make(sl.argv[1], T_Q);
			v->u.q.x = NUM(2); v->u.q.y = NUM(3); v->u.q.z = NUM(4); v->u.q.w = NUM(5);
		} else if (!strcmp(c, "quat_set_fn")) {
			struct var *v = make(sl.argv[1], T_Q);
			quat_set(&v->u.q, NUM(2), NUM(3), NUM(4), NUM(5));
		} else if (!strcmp(c, "aa")) {
			struct var *v = make(sl.argv[1], T_AA);
			axisang_set(&v->u.aa, NUM(2), NUM(3), NUM(4), NUM(5));
		} else if (!strcmp(c, "mat4")) {
			struct var *v = make(sl.argv[1], T_M4);
			float *p = (float *)&v->u.m;
			for (int i = 0; i < 16; i++)
				p[i] = NUM(2 + i);
		} else if (!strcmp(c, "show")) {
			show(A(1));
		} else if (!strcmp(c, "call")) {
			const char *fn = sl.argv[2];
			const char *dn = sl.argv[1];
			struct var *d;
#define V3OP2(name, call) if (!strcmp(fn, name)) { d = make(dn, T_V3); call(&d->u.v3, &A(3)->u.v3, &A(4)->u.v3); }
#define V3OPF(name, call) if (!strcmp(fn, name)) { d = make(dn, T_V3); call(&d->u.v3, &A(3)->u.v3, NUM(4)); }
#define V3OP1(name, call) if (!strcmp(fn, name)) { d = make(dn, T_V3); call(&d->u.v3, &A(3)->u.v3); }
#define V4OP2(name, call) if (!strcmp(fn, name)) { d = make(dn, T_V4); call(&d->u.v4, &A(3)->u.v4, &A(4)->u.v4); }
#define V4OPF(name, call) if (!strcmp(fn, name)) { d = make(dn, T_V4); call(&d->u.v4, &A(3)->u.v4, NUM(4)); }
#define V4OP1(name, call) if (!strcmp(fn, name)) { d = make(dn, T_V4); call(&d->u.v4, &A(3)->u.v4); }
#define QOP2(name, call) if (!strcmp(fn, name)) { d = make(dn, T_Q); call(&d->u.q, &A(3)->u.q, &A(4)->u.q); }
#define QOP1(name, call) if (!strcmp(fn, name)) { d = make(dn, T_Q); call(&d->u.q, &A(3)->u.q); }
#define M2(name, call) if (!strcmp(fn, name)) { d = make(dn, T_M4); call(&d->u.m, &A(3)->u.m, &A(4)->u.m); }
#define M1(name, call) if (!strcmp(fn, name)) { d = make(dn, T_M4); call(&d->u.m, &A(3)->u.m); }
			/* initialise destinations to a poison pattern so unwritten lanes are visible */
			V3OP2("vec3_add", vec3_add) else V3OP2("vec3_sub", vec3_sub) else V3OP2("vec3_mul", vec3_mul)
			else V3OP2("vec3_div", vec3_div) else V3OP2("vec3_cross", vec3_cross) else V3OP2("vec3_min", vec3_min)
			else V3OP2("vec3_max", vec3_max) else V3OPF("vec3_addf", vec3_addf) else V3OPF("vec3_subf", vec3_subf)
			else V3OPF("vec3_mulf", vec3_mulf) else V3OPF("vec3_divf", vec3_divf) else V3OPF("vec3_minf", vec3_minf)
			else V3OPF("vec3_maxf", vec3_maxf) else V3OP1("vec3_neg", vec3_neg) else V3OP1("vec3_norm", vec3_norm)
			else V3OP1("vec3_abs", vec3_abs) else V3OP1("vec3_floor", vec3_floor) else V3OP1("vec3_ceil", vec3_ceil)
			else if (!strcmp(fn, "vec3_dot")) { d = make(dn, T_F); d->u.f = vec3_dot(&A(3)->u.v3, &A(4)->u.v3); }
			else if (!strcmp(fn, "vec3_len")) { d = make(dn, T_F); d->u.f = vec3_len(&A(3)->u.v3); }
			else if (!strcmp(fn, "vec3_dist")) { d = make(dn, T_F); d->u.f = vec3_dist(&A(3)->u.v3, &A(4)->u.v3); }
			else if (!strcmp(fn, "vec3_close")) { d = make(dn, T_B); d->u.b = vec3_close(&A(3)->u.v3, &A(4)->u.v3, NUM(5)); }
			else if (!strcmp(fn, "vec3_transform")) { d = make(dn, T_V3); vec3_transform(&d->u.v3, &A(3)->u.v3, &A(4)->u.m); }
			else if (!strcmp(fn, "vec3_from_vec4")) { d = make(dn, T_V3); vec3_from_vec4(&d->u.v3, &A(3)->u.v4); }
			else V4OP2("vec4_add", vec4_add) else V4OP2("vec4_sub", vec4_sub) else V4OP2("vec4_mul", vec4_mul)
			else V4OP2("vec4_div", vec4_div) else V4OPF("vec4_mulf", vec4_mulf) else V4OP1("vec4_neg", vec4_neg)
			else V4OP1("vec4_norm", vec4_norm)
			else if (!strcmp(fn, "vec4_dot")) { d = make(dn, T_F); d->u.f = vec4_dot(&A(3)->u.v4, &A(4)->u.v4); }
			else if (!strcmp(fn, "vec4_len")) { d = make(dn, T_F); d->u.f = vec4_len(&A(3)->u.v4); }
			else if (!strcmp(fn, "vec4_close")) { d = make(dn, T_B); d->u.b = vec4_close(&A(3)->u.v4, &A(4)->u.v4, NUM(5)); }
			else if (!strcmp(fn, "vec4_transform")) { d = make(dn, T_V4); vec4_transform(&d->u.v4, &A(3)->u.v4, &A(4)->u.m); }
			else if (!strcmp(fn, "vec4_from_vec3")) { d = make(dn, T_V4); vec4_from_vec3(&d->u.v4, &A(3)->u.v3); }
			else if (!strcmp(fn, "vec4_to_rgba")) { d = make(dn, T_F); d->u.f = (float)vec4_to_rgba(&A(3)->u.v4); printf("%s rgba 0x%08x\n", dn, vec4_to_rgba(&A(3)->u.v4)); continue; }
			else QOP2("quat_mul", quat_mul) else QOP2("quat_add", quat_add) else QOP2("quat_sub", quat_sub)
			else QOP1("quat_inv", quat_inv) else QOP1("quat_neg", quat_neg) else QOP1("quat_norm", quat_norm)
			else QOP1("quat_log", quat_log) else QOP1("quat_exp", quat_exp)
			else if (!strcmp(fn, "quat_identity")) { d = make(dn, T_Q); quat_identity(&d->u.q); }
			else if (!strcmp(fn, "quat_dot")) { d = make(dn, T_F); d->u.f = quat_dot(&A(3)->u.q, &A(4)->u.q); }
			else if (!strcmp(fn, "quat_len")) { d = make(dn, T_F); d->u.f = quat_len(&A(3)->u.q); }
			else if (!strcmp(fn, "quat_dist")) { d = make(dn, T_F); d->u.f = quat_dist(&A(3)->u.q, &A(4)->u.q); }
			else if (!strcmp(fn, "quat_close")) { d = make(dn, T_B); d->u.b = quat_close(&A(3)->u.q, &A(4)->u.q, NUM(5)); }
			else if (!strcmp(fn, "quat_from_axisang")) { d = make(dn, T_Q); quat_from_axisang(&d->u.q, &A(3)->u.aa); }
			else if (!strcmp(fn, "quat_from_matrix4")) { d = make(dn, T_Q); quat_from_matrix4(&d->u.q, &A(3)->u.m); }
			else if (!strcmp(fn, "quat_get_dir")) { d = make(dn, T_V3); quat_get_dir(&d->u.v3, &A(3)->u.q); }
			else if (!strcmp(fn, "quat_set_look_dir")) { d = make(dn, T_Q); quat_set_look_dir(&d->u.q, &A(3)->u.v3); }
			else if (!strcmp(fn, "quat_interpolate")) { d = make(dn, T_Q); quat_interpolate(&d->u.q, &A(3)->u.q, &A(4)->u.q, NUM(5)); }
			else if (!strcmp(fn, "quat_get_tangent")) { d = make(dn, T_Q); quat_get_tangent(&d->u.q, &A(3)->u.q, &A(4)->u.q, &A(5)->u.q); }
			else if (!strcmp(fn, "quat_interpolate_cubic")) { d = make(dn, T_Q); quat_interpolate_cubic(&d->u.q, &A(3)->u.q, &A(4)->u.q, &A(5)->u.q, &A(6)->u.q, NUM(7)); }
			else M2("matrix4_mul", matrix4_mul) else M2("matrix4_mul_4x3_only", matrix4_mul_4x3_only)
			else M1("matrix4_transpose", matrix4_transpose)
			else if (!strcmp(fn, "matrix4_identity")) { d = make(dn, T_M4); matrix4_identity(&d->u.m); }
			else if (!strcmp(fn, "matrix4_determinant")) { d = make(dn, T_F); d->u.f = matrix4_determinant(&A(3)->u.m); }
			else if (!strcmp(fn, "matrix4_inv")) { d = make(dn, T_M4); memset(&d->u.m, 0x7f, sizeof d->u.m); bool ok = matrix4_inv(&d->u.m, &A(3)->u.m); printf("%s inv_ok %d\n", dn, ok); }
			else if (!strcmp(fn, "matrix4_from_quat")) { d = make(dn, T_M4); matrix4_from_quat(&d->u.m, &A(3)->u.q); }
			else if (!strcmp(fn, "matrix4_from_axisang")) { d = make(dn, T_M4); matrix4_from_axisang(&d->u.m, &A(3)->u.aa); }
			else if (!strcmp(fn, "matrix4_translate3v")) { d = make(dn, T_M4); matrix4_translate3v(&d->u.m, &A(3)->u.m, &A(4)->u.v3); }
			else if (!strcmp(fn, "matrix4_translate3v_i")) { d = make(dn, T_M4); matrix4_translate3v_i(&d->u.m, &A(3)->u.v3, &A(4)->u.m); }
			else if (!strcmp(fn, "matrix4_scale")) { d = make(dn, T_M4); matrix4_scale(&d->u.m, &A(3)->u.m, &A(4)->u.v3); }
			else if (!strcmp(fn, "matrix4_scale_i")) { d = make(dn, T_M4); matrix4_scale_i(&d->u.m, &A(3)->u.v3, &A(4)->u.m); }
			else if (!strcmp(fn, "matrix4_rotate")) { d = make(dn, T_M4); matrix4_rotate(&d->u.m, &A(3)->u.m, &A(4)->u.q); }
			else if (!strcmp(fn, "matrix4_rotate_aa")) { d = make(dn, T_M4); matrix4_rotate_aa(&d->u.m, &A(3)->u.m, &A(4)->u.aa); }
			else if (!strcmp(fn, "matrix4_rotate_i")) { d = make(dn, T_M4); matrix4_rotate_i(&d->u.m, &A(3)->u.q, &A(4)->u.m); }
			else { fprintf(stderr, "script: line %d: unknown function %s\n", sl.lineno, fn); return 2; }
			show(d);
		} else {
			fprintf(stderr, "script: line %d: unknown command '%s'\n", sl.lineno, c);
			return 2;
		}
	}
	fclose(f);
	return 0;
}
