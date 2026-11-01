/*
 * Conformance runner for vec2, matrix3, plane, bounds, math-extra, half, srgb, math-defs
 * and the vec3 functions that use them (spec S-009).  Test tooling, not part of libobs.
 *
 * Typed variables are stored by name (a call with an existing destination of the same type keeps
 * the destination's previous contents, so scripts can observe lanes/fields a function does NOT
 * write; new variables start zeroed; using the same name as source and destination aliases them).
 *
 *   float N v | vec2 N x y | vec3 N x y z [w] | quat N x y z w | aa N x y z angle
 *   mat3 N 16 floats | mat4 N 16 floats       rows x,y,z,t as listed (a matrix3 row has 4 lanes)
 *   plane N dx dy dz dist [dw] | bounds N minx miny minz maxx maxy maxz
 *   f32bits N 0xHEX                           float from a bit pattern
 *   px N HEX                                  byte buffer (RGBA texels), printed as hex
 *   f4 N a b c d                              four raw floats
 *   half_bits N 0xHHHH                        half from raw bits
 *   show N                                    print a variable
 *   call D fn args...                         D = fn(args); args are variable names or number literals
 *   rand_check positive N                     N calls of rand_float; prints range check only
 *   vec3_rand_check positive N                N calls of vec3_rand; prints range / w check only
 *
 * Output: floats %.9g, NaN is always printed as "nan". Bool results print as "bool 0|1".
 * Functions returning bool and an out parameter t print "hit <bool> <t>" where t was preset to
 * -7777 before the call (so "not written" is visible).
 */
#include <math.h>
#include <stdlib.h>
#include "graphics/vec2.h"
#include "graphics/vec3.h"
#include "graphics/quat.h"
#include "graphics/axisang.h"
#include "graphics/matrix3.h"
#include "graphics/matrix4.h"
#include "graphics/plane.h"
#include "graphics/bounds.h"
#include "graphics/math-extra.h"
#include "graphics/math-defs.h"
#include "graphics/half.h"
#include "graphics/srgb.h"
#include "harness.h"

enum { T_NONE, T_F, T_B, T_V2, T_V3, T_Q, T_AA, T_M3, T_M4, T_PL, T_BD, T_HIT, T_H, T_U8, T_F4, T_PX };
#define MAXPX 64
struct var {
	char name[32];
	int type;
	int maskw; /* print w lane of vec3 as '?' (reference leaves it uninitialised) */
	union {
		float f;
		int b;
		struct vec2 v2;
		struct vec3 v3;
		struct quat q;
		struct axisang aa;
		struct matrix3 m3;
		struct matrix4 m4;
		struct plane pl;
		struct bounds bd;
		struct {
			int b;
			float t;
		} hit;
		struct half h;
		unsigned u8;
		float f4[4];
		struct {
			int n;
			uint8_t d[MAXPX];
		} px;
	} u;
};
static struct var vars[128];

static struct var *get(const char *n)
{
	for (int i = 0; i < 128; i++)
		if (vars[i].type && !strcmp(vars[i].name, n))
			return &vars[i];
	return NULL;
}
static struct var *make(const char *n, int type)
{
	struct var *v = get(n);
	if (!v)
		for (int i = 0; i < 128; i++)
			if (!vars[i].type) {
				v = &vars[i];
				break;
			}
	if (v->type != type) {
		memset(v, 0, sizeof *v);
		snprintf(v->name, sizeof v->name, "%s", n);
		v->type = type;
	}
	v->maskw = 0;
	return v;
}
static struct var *need(const char *n, int type)
{
	struct var *v = get(n);
	if (!v || v->type != type) {
		fprintf(stderr, "script: variable '%s' missing or wrong type (%d)\n", n, type);
		exit(2);
	}
	return v;
}
static const char *fmtf(float f, char *buf)
{
	if (isnan(f))
		strcpy(buf, "nan");
	else
		sprintf(buf, "%.9g", f);
	return buf;
}
static void pf(const float *p, int n, int maskw_lane)
{
	char b[40];
	for (int i = 0; i < n; i++) {
		if (i == maskw_lane)
			printf(" ?");
		else
			printf(" %s", fmtf(p[i], b));
	}
}
static void show(const struct var *v)
{
	char b[40];
	printf("%s ", v->name);
	switch (v->type) {
	case T_F: printf("float %s\n", fmtf(v->u.f, b)); break;
	case T_B: printf("bool %d\n", v->u.b); break;
	case T_V2: printf("vec2"); pf(v->u.v2.ptr, 2, -1); printf("\n"); break;
	case T_V3: printf("vec3"); pf(v->u.v3.ptr, 4, v->maskw ? 3 : -1); printf("\n"); break;
	case T_Q: printf("quat"); pf(v->u.q.ptr, 4, -1); printf("\n"); break;
	case T_AA: printf("aa"); pf(v->u.aa.ptr, 4, -1); printf("\n"); break;
	case T_M3: printf("mat3"); pf((const float *)&v->u.m3, 16, -1); printf("\n"); break;
	case T_M4: printf("mat4"); pf((const float *)&v->u.m4, 16, -1); printf("\n"); break;
	case T_PL: printf("plane"); pf(v->u.pl.dir.ptr, 4, -1); printf(" %s\n", fmtf(v->u.pl.dist, b)); break;
	case T_BD: printf("bounds"); pf(v->u.bd.min.ptr, 4, -1); pf(v->u.bd.max.ptr, 4, -1); printf("\n"); break;
	case T_HIT: printf("hit %d %s\n", v->u.hit.b, fmtf(v->u.hit.t, b)); break;
	case T_H: printf("half 0x%04x\n", v->u.h.u); break;
	case T_U8: printf("u8 %u\n", v->u.u8); break;
	case T_F4: printf("f4"); pf(v->u.f4, 4, -1); printf("\n"); break;
	case T_PX:
		printf("px ");
		for (int i = 0; i < v->u.px.n; i++)
			printf("%02x", v->u.px.d[i]);
		printf("\n");
		break;
	}
}

#define NUM(i) ((float)strtod(sl.argv[i], NULL))
#define V2(i) (&need(sl.argv[i], T_V2)->u.v2)
#define V3(i) (&need(sl.argv[i], T_V3)->u.v3)
#define Q(i) (&need(sl.argv[i], T_Q)->u.q)
#define AA(i) (&need(sl.argv[i], T_AA)->u.aa)
#define M3(i) (&need(sl.argv[i], T_M3)->u.m3)
#define M4(i) (&need(sl.argv[i], T_M4)->u.m4)
#define PL(i) (&need(sl.argv[i], T_PL)->u.pl)
#define BD(i) (&need(sl.argv[i], T_BD)->u.bd)
#define F(i) (need(sl.argv[i], T_F)->u.f)
#define PX(i) (&need(sl.argv[i], T_PX)->u.px)

static int hexbytes(const char *s, uint8_t *out)
{
	int n = 0;
	while (s[0] && s[1] && n < MAXPX) {
		out[n++] = (uint8_t)(script_hexval(s[0]) * 16 + script_hexval(s[1]));
		s += 2;
	}
	return n;
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;
	while (script_next(f, &sl, &lineno)) {
		const char *c = sl.argv[0];
		if (!strcmp(c, "float")) {
			make(sl.argv[1], T_F)->u.f = NUM(2);
		} else if (!strcmp(c, "vec2")) {
			struct var *v = make(sl.argv[1], T_V2);
			vec2_set(&v->u.v2, NUM(2), NUM(3));
		} else if (!strcmp(c, "vec3")) {
			struct var *v = make(sl.argv[1], T_V3);
			v->u.v3.x = NUM(2); v->u.v3.y = NUM(3); v->u.v3.z = NUM(4);
			v->u.v3.w = sl.argc > 5 ? NUM(5) : 0.0f;
		} else if (!strcmp(c, "quat")) {
			struct var *v = make(sl.argv[1], T_Q);
			v->u.q.x = NUM(2); v->u.q.y = NUM(3); v->u.q.z = NUM(4); v->u.q.w = NUM(5);
		} else if (!strcmp(c, "aa")) {
			axisang_set(&make(sl.argv[1], T_AA)->u.aa, NUM(2), NUM(3), NUM(4), NUM(5));
		} else if (!strcmp(c, "mat3") || !strcmp(c, "mat4")) {
			struct var *v = make(sl.argv[1], c[3] == '3' ? T_M3 : T_M4);
			float *p = (float *)&v->u;
			for (int i = 0; i < 16; i++)
				p[i] = NUM(2 + i);
		} else if (!strcmp(c, "plane")) {
			struct var *v = make(sl.argv[1], T_PL);
			plane_setf(&v->u.pl, NUM(2), NUM(3), NUM(4), NUM(5));
			if (sl.argc > 6)
				v->u.pl.dir.w = NUM(6);
		} else if (!strcmp(c, "bounds")) {
			struct var *v = make(sl.argv[1], T_BD);
			vec3_set(&v->u.bd.min, NUM(2), NUM(3), NUM(4));
			vec3_set(&v->u.bd.max, NUM(5), NUM(6), NUM(7));
		} else if (!strcmp(c, "f32bits")) {
			struct var *v = make(sl.argv[1], T_F);
			uint32_t bits = (uint32_t)strtoul(sl.argv[2], NULL, 0);
			memcpy(&v->u.f, &bits, 4);
		} else if (!strcmp(c, "half_bits")) {
			make(sl.argv[1], T_H)->u.h = half_from_bits((uint16_t)strtoul(sl.argv[2], NULL, 0));
		} else if (!strcmp(c, "f4")) {
			struct var *v = make(sl.argv[1], T_F4);
			for (int i = 0; i < 4; i++)
				v->u.f4[i] = NUM(2 + i);
		} else if (!strcmp(c, "px")) {
			struct var *v = make(sl.argv[1], T_PX);
			v->u.px.n = hexbytes(sl.argv[2], v->u.px.d);
		} else if (!strcmp(c, "show")) {
			show(need(sl.argv[1], get(sl.argv[1]) ? get(sl.argv[1])->type : T_F));
		} else if (!strcmp(c, "rand_check") || !strcmp(c, "vec3_rand_check")) {
			int pos = atoi(sl.argv[1]), n = atoi(sl.argv[2]), ok = 1, varies = 0, wzero = 1;
			float lo = pos ? 0.0f : -1.0f, first = 0;
			srand(12345);
			for (int i = 0; i < n; i++) {
				float r[3];
				int k = 1;
				if (c[0] == 'v') {
					struct vec3 v;
					vec3_rand(&v, pos);
					r[0] = v.x; r[1] = v.y; r[2] = v.z;
					k = 3;
					wzero &= v.w == 0.0f;
				} else {
					r[0] = rand_float(pos);
				}
				for (int j = 0; j < k; j++) {
					ok &= r[j] >= lo && r[j] <= 1.0f;
					if (i == 0 && j == 0)
						first = r[0];
					else if (r[j] != first)
						varies = 1;
				}
			}
			printf("%s positive=%d n=%d in_range=%d varies=%d%s\n", c, pos, n, ok, varies,
			       c[0] == 'v' ? (wzero ? " w_zero=1" : " w_zero=0") : "");
		} else if (!strcmp(c, "call")) {
			const char *fn = sl.argv[2];
			const char *dn = sl.argv[1];
			struct var *d;
#define R_F(expr) { d = make(dn, T_F); d->u.f = (expr); }
#define R_B(expr) { d = make(dn, T_B); d->u.b = (expr) ? 1 : 0; }
#define R_V2() d = make(dn, T_V2)
#define R_V3() d = make(dn, T_V3)
#define IS(n) (!strcmp(fn, n))
			/* ---- math-defs ---- */
			if IS("close_float") R_B(close_float(NUM(3), NUM(4), NUM(5)))
			else if IS("RAD") R_F(RAD(NUM(3)))
			else if IS("DEG") R_F(DEG(NUM(3)))
			else if IS("const") {
				const char *k = sl.argv[3];
				d = make(dn, T_F);
				d->u.f = !strcmp(k, "EPSILON") ? EPSILON : !strcmp(k, "LARGE_EPSILON") ? LARGE_EPSILON
				       : !strcmp(k, "TINY_EPSILON") ? TINY_EPSILON : !strcmp(k, "M_INFINITE") ? M_INFINITE
				       : !strcmp(k, "RAD1") ? RAD(1.0f) : !strcmp(k, "DEG1") ? DEG(1.0f) : -1.0f;
			}
			/* ---- vec2 ---- */
			else if IS("vec2_add") { R_V2(); vec2_add(&d->u.v2, V2(3), V2(4)); }
			else if IS("vec2_sub") { R_V2(); vec2_sub(&d->u.v2, V2(3), V2(4)); }
			else if IS("vec2_mul") { R_V2(); vec2_mul(&d->u.v2, V2(3), V2(4)); }
			else if IS("vec2_div") { R_V2(); vec2_div(&d->u.v2, V2(3), V2(4)); }
			else if IS("vec2_addf") { R_V2(); vec2_addf(&d->u.v2, V2(3), NUM(4)); }
			else if IS("vec2_subf") { R_V2(); vec2_subf(&d->u.v2, V2(3), NUM(4)); }
			else if IS("vec2_mulf") { R_V2(); vec2_mulf(&d->u.v2, V2(3), NUM(4)); }
			else if IS("vec2_divf") { R_V2(); vec2_divf(&d->u.v2, V2(3), NUM(4)); }
			else if IS("vec2_neg") { R_V2(); vec2_neg(&d->u.v2, V2(3)); }
			else if IS("vec2_abs") { R_V2(); vec2_abs(&d->u.v2, V2(3)); }
			else if IS("vec2_floor") { R_V2(); vec2_floor(&d->u.v2, V2(3)); }
			else if IS("vec2_ceil") { R_V2(); vec2_ceil(&d->u.v2, V2(3)); }
			else if IS("vec2_norm") { R_V2(); vec2_norm(&d->u.v2, V2(3)); }
			else if IS("vec2_min") { R_V2(); vec2_min(&d->u.v2, V2(3), V2(4)); }
			else if IS("vec2_max") { R_V2(); vec2_max(&d->u.v2, V2(3), V2(4)); }
			else if IS("vec2_minf") { R_V2(); vec2_minf(&d->u.v2, V2(3), NUM(4)); }
			else if IS("vec2_maxf") { R_V2(); vec2_maxf(&d->u.v2, V2(3), NUM(4)); }
			else if IS("vec2_dot") R_F(vec2_dot(V2(3), V2(4)))
			else if IS("vec2_len") R_F(vec2_len(V2(3)))
			else if IS("vec2_dist") R_F(vec2_dist(V2(3), V2(4)))
			else if IS("vec2_close") R_B(vec2_close(V2(3), V2(4), NUM(5)))
			/* ---- vec3 functions using plane / matrix3 ---- */
			else if IS("vec3_plane_dist") R_F(vec3_plane_dist(V3(3), PL(4)))
			else if IS("vec3_rotate") { R_V3(); vec3_rotate(&d->u.v3, V3(3), M3(4)); }
			else if IS("vec3_transform3x4") { R_V3(); vec3_transform3x4(&d->u.v3, V3(3), M3(4)); }
			else if IS("vec3_mirror") { R_V3(); vec3_mirror(&d->u.v3, V3(3), PL(4)); }
			else if IS("vec3_mirrorv") { R_V3(); vec3_mirrorv(&d->u.v3, V3(3), V3(4)); }
			else if IS("vec3_close") R_B(vec3_close(V3(3), V3(4), NUM(5)))
			/* ---- matrix3 ---- */
			else if IS("matrix3_identity") { d = make(dn, T_M3); matrix3_identity(&d->u.m3); }
			else if IS("matrix3_copy") { d = make(dn, T_M3); matrix3_copy(&d->u.m3, M3(3)); }
			else if IS("matrix3_from_quat") { d = make(dn, T_M3); matrix3_from_quat(&d->u.m3, Q(3)); }
			else if IS("matrix3_from_axisang") { d = make(dn, T_M3); matrix3_from_axisang(&d->u.m3, AA(3)); }
			else if IS("matrix3_from_matrix4") { d = make(dn, T_M3); matrix3_from_matrix4(&d->u.m3, M4(3)); }
			else if IS("matrix3_mul") { d = make(dn, T_M3); matrix3_mul(&d->u.m3, M3(3), M3(4)); }
			else if IS("matrix3_translate") { d = make(dn, T_M3); matrix3_translate(&d->u.m3, M3(3), V3(4)); }
			else if IS("matrix3_translate3f") { d = make(dn, T_M3); matrix3_translate3f(&d->u.m3, M3(3), NUM(4), NUM(5), NUM(6)); }
			else if IS("matrix3_rotate") { d = make(dn, T_M3); matrix3_rotate(&d->u.m3, M3(3), Q(4)); }
			else if IS("matrix3_rotate_aa") { d = make(dn, T_M3); matrix3_rotate_aa(&d->u.m3, M3(3), AA(4)); }
			else if IS("matrix3_rotate_aa4f") { d = make(dn, T_M3); matrix3_rotate_aa4f(&d->u.m3, M3(3), NUM(4), NUM(5), NUM(6), NUM(7)); }
			else if IS("matrix3_scale") { d = make(dn, T_M3); matrix3_scale(&d->u.m3, M3(3), V3(4)); }
			else if IS("matrix3_scale3f") { d = make(dn, T_M3); matrix3_scale3f(&d->u.m3, M3(3), NUM(4), NUM(5), NUM(6)); }
			else if IS("matrix3_transpose") { d = make(dn, T_M3); matrix3_transpose(&d->u.m3, M3(3)); }
			else if IS("matrix3_inv") { d = make(dn, T_M3); matrix3_inv(&d->u.m3, M3(3)); }
			else if IS("matrix3_mirror") { d = make(dn, T_M3); matrix3_mirror(&d->u.m3, M3(3), PL(4)); }
			else if IS("matrix3_mirrorv") { d = make(dn, T_M3); matrix3_mirrorv(&d->u.m3, M3(3), V3(4)); }
			else if IS("matrix4_from_matrix3") { d = make(dn, T_M4); matrix4_from_matrix3(&d->u.m4, M3(3)); }
			else if IS("matrix4_mul") { d = make(dn, T_M4); matrix4_mul(&d->u.m4, M4(3), M4(4)); }
			else if IS("matrix4_inv") { d = make(dn, T_M4); matrix4_inv(&d->u.m4, M4(3)); }
			else if IS("matrix4_identity") { d = make(dn, T_M4); matrix4_identity(&d->u.m4); }
			else if IS("axisang_zero") { d = make(dn, T_AA); axisang_zero(&d->u.aa); }
			else if IS("axisang_copy") { d = make(dn, T_AA); axisang_copy(&d->u.aa, AA(3)); }
			else if IS("axisang_from_quat") { d = make(dn, T_AA); axisang_from_quat(&d->u.aa, Q(3)); }
			else if IS("quat_from_matrix3") { d = make(dn, T_Q); quat_from_matrix3(&d->u.q, M3(3)); }
			/* ---- plane ---- */
			else if IS("plane_from_tri") { d = make(dn, T_PL); plane_from_tri(&d->u.pl, V3(3), V3(4), V3(5)); }
			else if IS("plane_transform") { d = make(dn, T_PL); plane_transform(&d->u.pl, PL(3), M4(4)); }
			else if IS("plane_transform3x4") { d = make(dn, T_PL); plane_transform3x4(&d->u.pl, PL(3), M3(4)); }
			else if IS("plane_copy") { d = make(dn, T_PL); plane_copy(&d->u.pl, PL(3)); }
			else if IS("plane_set") { d = make(dn, T_PL); plane_set(&d->u.pl, V3(3), NUM(4)); }
			else if IS("plane_setf") { d = make(dn, T_PL); plane_setf(&d->u.pl, NUM(3), NUM(4), NUM(5), NUM(6)); }
			else if IS("plane_intersection_ray") {
				d = make(dn, T_HIT); d->u.hit.t = -7777.0f;
				d->u.hit.b = plane_intersection_ray(PL(3), V3(4), V3(5), &d->u.hit.t);
			} else if IS("plane_intersection_line") {
				d = make(dn, T_HIT); d->u.hit.t = -7777.0f;
				d->u.hit.b = plane_intersection_line(PL(3), V3(4), V3(5), &d->u.hit.t);
			}
			else if IS("plane_tri_inside") R_B(plane_tri_inside(PL(3), V3(4), V3(5), V3(6), NUM(7)))
			else if IS("plane_line_inside") R_B(plane_line_inside(PL(3), V3(4), V3(5), NUM(6)))
			else if IS("plane_close") R_B(plane_close(PL(3), PL(4), NUM(5)))
			else if IS("plane_coplanar") R_B(plane_coplanar(PL(3), PL(4), NUM(5)))
			/* ---- bounds ---- */
			else if IS("bounds_zero") { d = make(dn, T_BD); bounds_zero(&d->u.bd); }
			else if IS("bounds_copy") { d = make(dn, T_BD); bounds_copy(&d->u.bd, BD(3)); }
			else if IS("bounds_move") { d = make(dn, T_BD); bounds_move(&d->u.bd, BD(3), V3(4)); }
			else if IS("bounds_scale") { d = make(dn, T_BD); bounds_scale(&d->u.bd, BD(3), V3(4)); }
			else if IS("bounds_merge") { d = make(dn, T_BD); bounds_merge(&d->u.bd, BD(3), BD(4)); }
			else if IS("bounds_merge_point") { d = make(dn, T_BD); bounds_merge_point(&d->u.bd, BD(3), V3(4)); }
			else if IS("bounds_get_point") { R_V3(); bounds_get_point(&d->u.v3, BD(3), (unsigned)strtoul(sl.argv[4], NULL, 0)); }
			else if IS("bounds_get_center") { R_V3(); bounds_get_center(&d->u.v3, BD(3)); }
			else if IS("bounds_transform") { d = make(dn, T_BD); bounds_transform(&d->u.bd, BD(3), M4(4)); }
			else if IS("bounds_transform3x4") { d = make(dn, T_BD); bounds_transform3x4(&d->u.bd, BD(3), M3(4)); }
			else if IS("bounds_intersection_ray") {
				d = make(dn, T_HIT); d->u.hit.t = -7777.0f;
				d->u.hit.b = bounds_intersection_ray(BD(3), V3(4), V3(5), &d->u.hit.t);
			} else if IS("bounds_intersection_line") {
				d = make(dn, T_HIT); d->u.hit.t = -7777.0f;
				d->u.hit.b = bounds_intersection_line(BD(3), V3(4), V3(5), &d->u.hit.t);
			}
			else if IS("bounds_plane_test") R_B(bounds_plane_test(BD(3), PL(4)))
			else if IS("bounds_under_plane") R_B(bounds_under_plane(BD(3), PL(4)))
			else if IS("bounds_inside") R_B(bounds_inside(BD(3), BD(4)))
			else if IS("bounds_vec3_inside") R_B(bounds_vec3_inside(BD(3), V3(4)))
			else if IS("bounds_intersects") R_B(bounds_intersects(BD(3), BD(4), NUM(5)))
			else if IS("bounds_intersects_obb") R_B(bounds_intersects_obb(BD(3), BD(4), M4(5), NUM(6)))
			else if IS("bounds_intersects_obb3x4") R_B(bounds_intersects_obb3x4(BD(3), BD(4), M3(5), NUM(6)))
			else if IS("bounds_intersects_ray") R_B(bounds_intersects_ray(BD(3), V3(4), V3(5)))
			else if IS("bounds_intersects_line") R_B(bounds_intersects_line(BD(3), V3(4), V3(5)))
			else if IS("bounds_min_dist") R_F(bounds_min_dist(BD(3), PL(4)))
			/* ---- math-extra ---- */
			else if IS("polar_to_cart") { R_V3(); polar_to_cart(&d->u.v3, V3(3)); d->maskw = 1; }
			else if IS("cart_to_polar") { R_V3(); cart_to_polar(&d->u.v3, V3(3)); d->maskw = 1; }
			else if IS("norm_to_polar") { R_V2(); norm_to_polar(&d->u.v2, V3(3)); }
			else if IS("polar_to_norm") { R_V3(); polar_to_norm(&d->u.v3, V2(3)); }
			else if IS("calc_torquef") R_F(calc_torquef(NUM(3), NUM(4), NUM(5), NUM(6), NUM(7)))
			else if IS("calc_torque") { R_V3(); calc_torque(&d->u.v3, V3(3), V3(4), NUM(5), NUM(6), NUM(7)); }
			else if IS("get_percentage") R_F(get_percentage(NUM(3), NUM(4), NUM(5)))
			else if IS("get_percentagei") R_F(get_percentagei(atoi(sl.argv[3]), atoi(sl.argv[4]), atoi(sl.argv[5])))
			/* ---- half ---- */
			else if IS("half_from_float") { d = make(dn, T_H); d->u.h = half_from_float(NUM(3)); }
			else if IS("half_from_float_var") { d = make(dn, T_H); d->u.h = half_from_float(F(3)); }
			/* ---- srgb ---- */
			else if IS("srgb_nonlinear_to_linear") R_F(gs_srgb_nonlinear_to_linear(NUM(3)))
			else if IS("srgb_linear_to_nonlinear") R_F(gs_srgb_linear_to_nonlinear(NUM(3)))
			else if IS("u8_to_float") R_F(gs_u8_to_float((uint8_t)atoi(sl.argv[3])))
			else if IS("float_to_u8") { d = make(dn, T_U8); d->u.u8 = gs_float_to_u8(NUM(3)); }
			else if IS("u8x4_to_float4") {
				d = make(dn, T_F4);
				gs_u8x4_to_float4(d->u.f4, PX(3)->d);
			} else if IS("float4_to_u8x4") {
				d = make(dn, T_PX); d->u.px.n = 4;
				gs_float4_to_u8x4(d->u.px.d, need(sl.argv[3], T_F4)->u.f4);
			} else if IS("float3_to_u8x3") {
				d = make(dn, T_PX); d->u.px.n = 4; /* 4th byte keeps its previous value */
				gs_float3_to_u8x3(d->u.px.d, need(sl.argv[3], T_F4)->u.f4);
			} else if IS("premultiply_float4") {
				d = make(dn, T_F4);
				memcpy(d->u.f4, need(sl.argv[3], T_F4)->u.f4, 16);
				gs_premultiply_float4(d->u.f4);
			} else if IS("float3_srgb_nonlinear_to_linear") {
				d = make(dn, T_F4);
				memcpy(d->u.f4, need(sl.argv[3], T_F4)->u.f4, 16);
				gs_float3_srgb_nonlinear_to_linear(d->u.f4);
			} else if IS("float3_srgb_linear_to_nonlinear") {
				d = make(dn, T_F4);
				memcpy(d->u.f4, need(sl.argv[3], T_F4)->u.f4, 16);
				gs_float3_srgb_linear_to_nonlinear(d->u.f4);
			} else if (!strncmp(fn, "premultiply_xyza", 16)) {
				/* in-place forms copy the source into D first; restrict forms write into D */
				struct var *s = need(sl.argv[3], T_PX);
				int cnt = sl.argc > 4 ? atoi(sl.argv[4]) : 0;
				d = make(dn, T_PX);
				d->u.px.n = s->u.px.n;
				if (!strstr(fn, "restrict"))
					memcpy(d->u.px.d, s->u.px.d, s->u.px.n);
				if IS("premultiply_xyza") gs_premultiply_xyza(d->u.px.d);
				else if IS("premultiply_xyza_srgb") gs_premultiply_xyza_srgb(d->u.px.d);
				else if IS("premultiply_xyza_restrict") gs_premultiply_xyza_restrict(d->u.px.d, s->u.px.d);
				else if IS("premultiply_xyza_srgb_restrict") gs_premultiply_xyza_srgb_restrict(d->u.px.d, s->u.px.d);
				else if IS("premultiply_xyza_loop") gs_premultiply_xyza_loop(d->u.px.d, (size_t)cnt);
				else if IS("premultiply_xyza_srgb_loop") gs_premultiply_xyza_srgb_loop(d->u.px.d, (size_t)cnt);
				else if IS("premultiply_xyza_loop_restrict") gs_premultiply_xyza_loop_restrict(d->u.px.d, s->u.px.d, (size_t)cnt);
				else if IS("premultiply_xyza_srgb_loop_restrict") gs_premultiply_xyza_srgb_loop_restrict(d->u.px.d, s->u.px.d, (size_t)cnt);
				else { fprintf(stderr, "script: line %d: unknown function %s\n", sl.lineno, fn); return 2; }
			}
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
