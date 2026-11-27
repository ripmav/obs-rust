/*
 * obs-ref-dump (WP-39, P0-14.2): math dump.
 *
 * Prints the deterministic reference output of EVERY function in the pinned
 * libobs 33.0.1 graphics math library (vec2/vec3/vec4/matrix3/matrix4/quat/
 * axisang/plane/bounds/math-extra — both .c definitions and header static
 * inline / EXPORT functions). 32 seeded cases per function (splitmix64,
 * seed 0x0B5, see ref-dump.c), floats printed with %.9g.
 *
 * Determinism notes:
 *  - No aliasing: dst and src buffers are always distinct.
 *  - No time/env input. The only exception is the final rand() section
 *    (libc rand is platform-dependent); it starts with "# section: rand"
 *    and is last, so the CI diff excludes everything from that line on.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "graphics/vec2.h"
#include "graphics/vec3.h"
#include "graphics/vec4.h"
#include "graphics/matrix3.h"
#include "graphics/matrix4.h"
#include "graphics/quat.h"
#include "graphics/axisang.h"
#include "graphics/plane.h"
#include "graphics/bounds.h"
#include "graphics/math-extra.h"

/* PRNG exposed by ref-dump.c */
extern uint32_t next_u32(void);
extern void rd_reset_prng(void);

#define CASES 32

/* --- random value helpers (all from the splitmix64 stream) --- */
static float r01(void)
{
	return (float)((double)next_u32() * (1.0 / 4294967296.0));
}
static float rr(void) /* [-1,1) */
{
	return r01() * 2.0f - 1.0f;
}
static struct vec2 rv2(void)
{
	struct vec2 v;
	v.x = rr();
	v.y = rr();
	return v;
}
static struct vec3 rv3(void)
{
	struct vec3 v;
	v.x = rr();
	v.y = rr();
	v.z = rr();
	/* struct vec3 is 16 bytes with an unused w field (a C quirk:
	 * matrix4_from_matrix3 copies it); keep it at a defined value */
	v.w = 0.0f;
	return v;
}
static struct vec4 rv4(void)
{
	struct vec4 v;
	v.x = rr();
	v.y = rr();
	v.z = rr();
	v.w = rr();
	return v;
}
static struct quat rq(void)
{
	struct quat q;
	q.x = rr();
	q.y = rr();
	q.z = rr();
	q.w = rr();
	return q;
}
static struct axisang raa(void)
{
	struct axisang a;
	a.x = rr();
	a.y = rr();
	a.z = rr();
	a.w = rr();
	return a;
}
static struct matrix3 rm3(void)
{
	struct matrix3 m;
	m.x = rv3();
	m.y = rv3();
	m.z = rv3();
	m.t = rv3();
	return m;
}
static struct matrix4 rm4(void)
{
	struct matrix4 m;
	m.x = rv4();
	m.y = rv4();
	m.z = rv4();
	m.t = rv4();
	return m;
}
static struct plane rpl(void)
{
	struct plane p;
	p.dir = rv3();
	p.dist = rr();
	return p;
}
static struct bounds rbd(void)
{
	struct bounds b;
	b.min = rv3();
	b.max = rv3();
	return b;
}

/* --- formatting (local buffers only: no shared static state) --- */
#define P2(v) (v).x, (v).y
#define P3(v) (v).x, (v).y, (v).z
#define P4(v) (v).x, (v).y, (v).z, (v).w
#define P2S(v) "%.9g %.9g"
#define P3S(v) "%.9g %.9g %.9g"
#define P4S(v) "%.9g %.9g %.9g %.9g"

static const char *pm3(const struct matrix3 *m)
{
	/* ring of buffers: a single printf may hold several matrices at once */
	static char bufs[4][512];
	static int idx;
	char *buf = bufs[idx++ & 3];
	snprintf(buf, sizeof(bufs[0]), "x(%.9g %.9g %.9g) y(%.9g %.9g %.9g) z(%.9g %.9g %.9g) t(%.9g %.9g %.9g)", P3(m->x),
		 P3(m->y), P3(m->z), P3(m->t));
	return buf;
}
static const char *pm4(const struct matrix4 *m)
{
	static char bufs[4][512];
	static int idx;
	char *buf = bufs[idx++ & 3];
	snprintf(buf, sizeof(bufs[0]), "x(%.9g %.9g %.9g %.9g) y(%.9g %.9g %.9g %.9g) z(%.9g %.9g %.9g %.9g) "
					 "t(%.9g %.9g %.9g %.9g)",
		 P4(m->x), P4(m->y), P4(m->z), P4(m->t));
	return buf;
}

void obs_ref_dump_math(void)
{
	rd_reset_prng();

	/* ================= vec2 ================= */
	{
		printf("# math/vec2_zero\n");
		for (int c = 0; c < CASES; c++) {
			struct vec2 d = {0};
			vec2_zero(&d);
			printf("case %d: -> %.9g %.9g\n", c, P2(d));
		}
	}
	{
		printf("# math/vec2_set\n");
		for (int c = 0; c < CASES; c++) {
			struct vec2 d = {0};
			float x = rr(), y = rr();
			vec2_set(&d, x, y);
			printf("case %d: (%.9g %.9g) -> %.9g %.9g\n", c, x, y, P2(d));
		}
	}
	{
		printf("# math/vec2_copy\n");
		for (int c = 0; c < CASES; c++) {
			struct vec2 v = rv2(), d = {0};
			vec2_copy(&d, &v);
			printf("case %d: (%.9g %.9g) -> %.9g %.9g\n", c, P2(v), P2(d));
		}
	}
	{
		printf("# math/vec2_add\n");
		for (int c = 0; c < CASES; c++) {
			struct vec2 a = rv2(), b = rv2(), d = {0};
			vec2_add(&d, &a, &b);
			printf("case %d: a(%.9g %.9g) b(%.9g %.9g) -> %.9g %.9g\n", c, P2(a), P2(b), P2(d));
		}
	}
	{
		printf("# math/vec2_sub\n");
		for (int c = 0; c < CASES; c++) {
			struct vec2 a = rv2(), b = rv2(), d = {0};
			vec2_sub(&d, &a, &b);
			printf("case %d: a(%.9g %.9g) b(%.9g %.9g) -> %.9g %.9g\n", c, P2(a), P2(b), P2(d));
		}
	}
	{
		printf("# math/vec2_mul\n");
		for (int c = 0; c < CASES; c++) {
			struct vec2 a = rv2(), b = rv2(), d = {0};
			vec2_mul(&d, &a, &b);
			printf("case %d: a(%.9g %.9g) b(%.9g %.9g) -> %.9g %.9g\n", c, P2(a), P2(b), P2(d));
		}
	}
	{
		printf("# math/vec2_div\n");
		for (int c = 0; c < CASES; c++) {
			struct vec2 a = rv2(), b = rv2(), d = {0};
			vec2_div(&d, &a, &b);
			printf("case %d: a(%.9g %.9g) b(%.9g %.9g) -> %.9g %.9g\n", c, P2(a), P2(b), P2(d));
		}
	}
	{
		printf("# math/vec2_addf\n");
		for (int c = 0; c < CASES; c++) {
			struct vec2 v = rv2(), d = {0};
			float f = rr();
			vec2_addf(&d, &v, f);
			printf("case %d: v(%.9g %.9g) f=%.9g -> %.9g %.9g\n", c, P2(v), f, P2(d));
		}
	}
	{
		printf("# math/vec2_subf\n");
		for (int c = 0; c < CASES; c++) {
			struct vec2 v = rv2(), d = {0};
			float f = rr();
			vec2_subf(&d, &v, f);
			printf("case %d: v(%.9g %.9g) f=%.9g -> %.9g %.9g\n", c, P2(v), f, P2(d));
		}
	}
	{
		printf("# math/vec2_mulf\n");
		for (int c = 0; c < CASES; c++) {
			struct vec2 v = rv2(), d = {0};
			float f = rr();
			vec2_mulf(&d, &v, f);
			printf("case %d: v(%.9g %.9g) f=%.9g -> %.9g %.9g\n", c, P2(v), f, P2(d));
		}
	}
	{
		printf("# math/vec2_divf\n");
		for (int c = 0; c < CASES; c++) {
			struct vec2 v = rv2(), d = {0};
			float f = rr();
			vec2_divf(&d, &v, f);
			printf("case %d: v(%.9g %.9g) f=%.9g -> %.9g %.9g\n", c, P2(v), f, P2(d));
		}
	}
	{
		printf("# math/vec2_neg\n");
		for (int c = 0; c < CASES; c++) {
			struct vec2 v = rv2(), d = {0};
			vec2_neg(&d, &v);
			printf("case %d: v(%.9g %.9g) -> %.9g %.9g\n", c, P2(v), P2(d));
		}
	}
	{
		printf("# math/vec2_dot\n");
		for (int c = 0; c < CASES; c++) {
			struct vec2 a = rv2(), b = rv2();
			float r = vec2_dot(&a, &b);
			printf("case %d: a(%.9g %.9g) b(%.9g %.9g) -> %.9g\n", c, P2(a), P2(b), r);
		}
	}
	{
		printf("# math/vec2_len\n");
		for (int c = 0; c < CASES; c++) {
			struct vec2 v = rv2();
			float r = vec2_len(&v);
			printf("case %d: v(%.9g %.9g) -> %.9g\n", c, P2(v), r);
		}
	}
	{
		printf("# math/vec2_dist\n");
		for (int c = 0; c < CASES; c++) {
			struct vec2 a = rv2(), b = rv2();
			float r = vec2_dist(&a, &b);
			printf("case %d: a(%.9g %.9g) b(%.9g %.9g) -> %.9g\n", c, P2(a), P2(b), r);
		}
	}
	{
		printf("# math/vec2_minf\n");
		for (int c = 0; c < CASES; c++) {
			struct vec2 v = rv2(), d = {0};
			float f = rr();
			vec2_minf(&d, &v, f);
			printf("case %d: v(%.9g %.9g) f=%.9g -> %.9g %.9g\n", c, P2(v), f, P2(d));
		}
	}
	{
		printf("# math/vec2_min\n");
		for (int c = 0; c < CASES; c++) {
			struct vec2 v = rv2(), minv = rv2(), d = {0};
			vec2_min(&d, &v, &minv);
			printf("case %d: v(%.9g %.9g) min(%.9g %.9g) -> %.9g %.9g\n", c, P2(v), P2(minv), P2(d));
		}
	}
	{
		printf("# math/vec2_maxf\n");
		for (int c = 0; c < CASES; c++) {
			struct vec2 v = rv2(), d = {0};
			float f = rr();
			vec2_maxf(&d, &v, f);
			printf("case %d: v(%.9g %.9g) f=%.9g -> %.9g %.9g\n", c, P2(v), f, P2(d));
		}
	}
	{
		printf("# math/vec2_max\n");
		for (int c = 0; c < CASES; c++) {
			struct vec2 v = rv2(), maxv = rv2(), d = {0};
			vec2_max(&d, &v, &maxv);
			printf("case %d: v(%.9g %.9g) max(%.9g %.9g) -> %.9g %.9g\n", c, P2(v), P2(maxv), P2(d));
		}
	}
	{
		printf("# math/vec2_abs\n");
		for (int c = 0; c < CASES; c++) {
			struct vec2 v = rv2(), d = {0};
			vec2_abs(&d, &v);
			printf("case %d: v(%.9g %.9g) -> %.9g %.9g\n", c, P2(v), P2(d));
		}
	}
	{
		printf("# math/vec2_floor\n");
		for (int c = 0; c < CASES; c++) {
			struct vec2 v = rv2(), d = {0};
			vec2_floor(&d, &v);
			printf("case %d: v(%.9g %.9g) -> %.9g %.9g\n", c, P2(v), P2(d));
		}
	}
	{
		printf("# math/vec2_ceil\n");
		for (int c = 0; c < CASES; c++) {
			struct vec2 v = rv2(), d = {0};
			vec2_ceil(&d, &v);
			printf("case %d: v(%.9g %.9g) -> %.9g %.9g\n", c, P2(v), P2(d));
		}
	}
	{
		printf("# math/vec2_close\n");
		for (int c = 0; c < CASES; c++) {
			struct vec2 a = rv2(), b = rv2();
			float eps = rr();
			int r = vec2_close(&a, &b, eps);
			printf("case %d: a(%.9g %.9g) b(%.9g %.9g) eps=%.9g -> %d\n", c, P2(a), P2(b), eps, r);
		}
	}
	{
		printf("# math/vec2_norm\n");
		for (int c = 0; c < CASES; c++) {
			struct vec2 v = rv2(), d = {0};
			vec2_norm(&d, &v);
			printf("case %d: v(%.9g %.9g) -> %.9g %.9g\n", c, P2(v), P2(d));
		}
	}

	/* ================= vec3 ================= */
	{
		printf("# math/vec3_zero\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 d = {0};
			vec3_zero(&d);
			printf("case %d: -> %.9g %.9g %.9g\n", c, P3(d));
		}
	}
	{
		printf("# math/vec3_set\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 d = {0};
			float x = rr(), y = rr(), z = rr();
			vec3_set(&d, x, y, z);
			printf("case %d: (%.9g %.9g %.9g) -> %.9g %.9g %.9g\n", c, x, y, z, P3(d));
		}
	}
	{
		printf("# math/vec3_copy\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 v = rv3(), d = {0};
			vec3_copy(&d, &v);
			printf("case %d: (%.9g %.9g %.9g) -> %.9g %.9g %.9g\n", c, P3(v), P3(d));
		}
	}
	{
		printf("# math/vec3_from_vec4\n");
		for (int c = 0; c < CASES; c++) {
			struct vec4 v = rv4();
			struct vec3 d = {0};
			vec3_from_vec4(&d, &v);
			printf("case %d: v4(%.9g %.9g %.9g %.9g) -> %.9g %.9g %.9g\n", c, P4(v), P3(d));
		}
	}
	{
		printf("# math/vec3_add\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 a = rv3(), b = rv3(), d = {0};
			vec3_add(&d, &a, &b);
			printf("case %d: a(%.9g %.9g %.9g) b(%.9g %.9g %.9g) -> %.9g %.9g %.9g\n", c, P3(a), P3(b), P3(d));
		}
	}
	{
		printf("# math/vec3_sub\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 a = rv3(), b = rv3(), d = {0};
			vec3_sub(&d, &a, &b);
			printf("case %d: a(%.9g %.9g %.9g) b(%.9g %.9g %.9g) -> %.9g %.9g %.9g\n", c, P3(a), P3(b), P3(d));
		}
	}
	{
		printf("# math/vec3_mul\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 a = rv3(), b = rv3(), d = {0};
			vec3_mul(&d, &a, &b);
			printf("case %d: a(%.9g %.9g %.9g) b(%.9g %.9g %.9g) -> %.9g %.9g %.9g\n", c, P3(a), P3(b), P3(d));
		}
	}
	{
		printf("# math/vec3_div\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 a = rv3(), b = rv3(), d = {0};
			vec3_div(&d, &a, &b);
			printf("case %d: a(%.9g %.9g %.9g) b(%.9g %.9g %.9g) -> %.9g %.9g %.9g\n", c, P3(a), P3(b), P3(d));
		}
	}
	{
		printf("# math/vec3_addf\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 v = rv3(), d = {0};
			float f = rr();
			vec3_addf(&d, &v, f);
			printf("case %d: v(%.9g %.9g %.9g) f=%.9g -> %.9g %.9g %.9g\n", c, P3(v), f, P3(d));
		}
	}
	{
		printf("# math/vec3_subf\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 v = rv3(), d = {0};
			float f = rr();
			vec3_subf(&d, &v, f);
			printf("case %d: v(%.9g %.9g %.9g) f=%.9g -> %.9g %.9g %.9g\n", c, P3(v), f, P3(d));
		}
	}
	{
		printf("# math/vec3_mulf\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 v = rv3(), d = {0};
			float f = rr();
			vec3_mulf(&d, &v, f);
			printf("case %d: v(%.9g %.9g %.9g) f=%.9g -> %.9g %.9g %.9g\n", c, P3(v), f, P3(d));
		}
	}
	{
		printf("# math/vec3_divf\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 v = rv3(), d = {0};
			float f = rr();
			vec3_divf(&d, &v, f);
			printf("case %d: v(%.9g %.9g %.9g) f=%.9g -> %.9g %.9g %.9g\n", c, P3(v), f, P3(d));
		}
	}
	{
		printf("# math/vec3_dot\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 a = rv3(), b = rv3();
			float r = vec3_dot(&a, &b);
			printf("case %d: a(%.9g %.9g %.9g) b(%.9g %.9g %.9g) -> %.9g\n", c, P3(a), P3(b), r);
		}
	}
	{
		printf("# math/vec3_cross\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 a = rv3(), b = rv3(), d = {0};
			vec3_cross(&d, &a, &b);
			printf("case %d: a(%.9g %.9g %.9g) b(%.9g %.9g %.9g) -> %.9g %.9g %.9g\n", c, P3(a), P3(b), P3(d));
		}
	}
	{
		printf("# math/vec3_neg\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 v = rv3(), d = {0};
			vec3_neg(&d, &v);
			printf("case %d: v(%.9g %.9g %.9g) -> %.9g %.9g %.9g\n", c, P3(v), P3(d));
		}
	}
	{
		printf("# math/vec3_len\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 v = rv3();
			float r = vec3_len(&v);
			printf("case %d: v(%.9g %.9g %.9g) -> %.9g\n", c, P3(v), r);
		}
	}
	{
		printf("# math/vec3_dist\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 a = rv3(), b = rv3();
			float r = vec3_dist(&a, &b);
			printf("case %d: a(%.9g %.9g %.9g) b(%.9g %.9g %.9g) -> %.9g\n", c, P3(a), P3(b), r);
		}
	}
	{
		printf("# math/vec3_norm\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 v = rv3(), d = {0};
			vec3_norm(&d, &v);
			printf("case %d: v(%.9g %.9g %.9g) -> %.9g %.9g %.9g\n", c, P3(v), P3(d));
		}
	}
	{
		printf("# math/vec3_close\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 a = rv3(), b = rv3();
			float eps = rr();
			int r = vec3_close(&a, &b, eps);
			printf("case %d: a(%.9g %.9g %.9g) b(%.9g %.9g %.9g) eps=%.9g -> %d\n", c, P3(a), P3(b), eps, r);
		}
	}
	{
		printf("# math/vec3_min\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 v = rv3(), minv = rv3(), d = {0};
			vec3_min(&d, &v, &minv);
			printf("case %d: v(%.9g %.9g %.9g) min(%.9g %.9g %.9g) -> %.9g %.9g %.9g\n", c, P3(v), P3(minv), P3(d));
		}
	}
	{
		printf("# math/vec3_minf\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 v = rv3(), d = {0};
			float f = rr();
			vec3_minf(&d, &v, f);
			printf("case %d: v(%.9g %.9g %.9g) f=%.9g -> %.9g %.9g %.9g\n", c, P3(v), f, P3(d));
		}
	}
	{
		printf("# math/vec3_max\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 v = rv3(), maxv = rv3(), d = {0};
			vec3_max(&d, &v, &maxv);
			printf("case %d: v(%.9g %.9g %.9g) max(%.9g %.9g %.9g) -> %.9g %.9g %.9g\n", c, P3(v), P3(maxv), P3(d));
		}
	}
	{
		printf("# math/vec3_maxf\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 v = rv3(), d = {0};
			float f = rr();
			vec3_maxf(&d, &v, f);
			printf("case %d: v(%.9g %.9g %.9g) f=%.9g -> %.9g %.9g %.9g\n", c, P3(v), f, P3(d));
		}
	}
	{
		printf("# math/vec3_abs\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 v = rv3(), d = {0};
			vec3_abs(&d, &v);
			printf("case %d: v(%.9g %.9g %.9g) -> %.9g %.9g %.9g\n", c, P3(v), P3(d));
		}
	}
	{
		printf("# math/vec3_floor\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 v = rv3(), d = {0};
			vec3_floor(&d, &v);
			printf("case %d: v(%.9g %.9g %.9g) -> %.9g %.9g %.9g\n", c, P3(v), P3(d));
		}
	}
	{
		printf("# math/vec3_ceil\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 v = rv3(), d = {0};
			vec3_ceil(&d, &v);
			printf("case %d: v(%.9g %.9g %.9g) -> %.9g %.9g %.9g\n", c, P3(v), P3(d));
		}
	}
	{
		printf("# math/vec3_plane_dist\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 v = rv3();
			struct plane p = rpl();
			float r = vec3_plane_dist(&v, &p);
			printf("case %d: v(%.9g %.9g %.9g) p(dir(%.9g %.9g %.9g) dist=%.9g) -> %.9g\n", c, P3(v), P3(p.dir), p.dist, r);
		}
	}
	{
		printf("# math/vec3_transform\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 v = rv3(), d = {0};
			struct matrix4 m = rm4();
			vec3_transform(&d, &v, &m);
			printf("case %d: v(%.9g %.9g %.9g) m(%s) -> %.9g %.9g %.9g\n", c, P3(v), pm4(&m), P3(d));
		}
	}
	{
		printf("# math/vec3_rotate\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 v = rv3(), d = {0};
			struct matrix3 m = rm3();
			vec3_rotate(&d, &v, &m);
			printf("case %d: v(%.9g %.9g %.9g) m(%s) -> %.9g %.9g %.9g\n", c, P3(v), pm3(&m), P3(d));
		}
	}
	{
		printf("# math/vec3_transform3x4\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 v = rv3(), d = {0};
			struct matrix3 m = rm3();
			vec3_transform3x4(&d, &v, &m);
			printf("case %d: v(%.9g %.9g %.9g) m(%s) -> %.9g %.9g %.9g\n", c, P3(v), pm3(&m), P3(d));
		}
	}
	{
		printf("# math/vec3_mirror\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 v = rv3(), d = {0};
			struct plane p = rpl();
			vec3_mirror(&d, &v, &p);
			printf("case %d: v(%.9g %.9g %.9g) p(dir(%.9g %.9g %.9g) dist=%.9g) -> %.9g %.9g %.9g\n", c, P3(v), P3(p.dir), p.dist, P3(d));
		}
	}
	{
		printf("# math/vec3_mirrorv\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 v = rv3(), vec = rv3(), d = {0};
			vec3_mirrorv(&d, &v, &vec);
			printf("case %d: v(%.9g %.9g %.9g) vec(%.9g %.9g %.9g) -> %.9g %.9g %.9g\n", c, P3(v), P3(vec), P3(d));
		}
	}

	/* ================= vec4 ================= */
	{
		printf("# math/vec4_zero\n");
		for (int c = 0; c < CASES; c++) {
			struct vec4 d = {0};
			vec4_zero(&d);
			printf("case %d: -> %.9g %.9g %.9g %.9g\n", c, P4(d));
		}
	}
	{
		printf("# math/vec4_set\n");
		for (int c = 0; c < CASES; c++) {
			struct vec4 d = {0};
			float x = rr(), y = rr(), z = rr(), w = rr();
			vec4_set(&d, x, y, z, w);
			printf("case %d: (%.9g %.9g %.9g %.9g) -> %.9g %.9g %.9g %.9g\n", c, x, y, z, w, P4(d));
		}
	}
	{
		printf("# math/vec4_copy\n");
		for (int c = 0; c < CASES; c++) {
			struct vec4 v = rv4(), d = {0};
			vec4_copy(&d, &v);
			printf("case %d: (%.9g %.9g %.9g %.9g) -> %.9g %.9g %.9g %.9g\n", c, P4(v), P4(d));
		}
	}
	{
		printf("# math/vec4_from_vec3\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 v = rv3();
			struct vec4 d = {0};
			vec4_from_vec3(&d, &v);
			printf("case %d: v3(%.9g %.9g %.9g) -> %.9g %.9g %.9g %.9g\n", c, P3(v), P4(d));
		}
	}
	{
		printf("# math/vec4_add\n");
		for (int c = 0; c < CASES; c++) {
			struct vec4 a = rv4(), b = rv4(), d = {0};
			vec4_add(&d, &a, &b);
			printf("case %d: a(%.9g %.9g %.9g %.9g) b(%.9g %.9g %.9g %.9g) -> %.9g %.9g %.9g %.9g\n", c, P4(a), P4(b), P4(d));
		}
	}
	{
		printf("# math/vec4_sub\n");
		for (int c = 0; c < CASES; c++) {
			struct vec4 a = rv4(), b = rv4(), d = {0};
			vec4_sub(&d, &a, &b);
			printf("case %d: a(%.9g %.9g %.9g %.9g) b(%.9g %.9g %.9g %.9g) -> %.9g %.9g %.9g %.9g\n", c, P4(a), P4(b), P4(d));
		}
	}
	{
		printf("# math/vec4_mul\n");
		for (int c = 0; c < CASES; c++) {
			struct vec4 a = rv4(), b = rv4(), d = {0};
			vec4_mul(&d, &a, &b);
			printf("case %d: a(%.9g %.9g %.9g %.9g) b(%.9g %.9g %.9g %.9g) -> %.9g %.9g %.9g %.9g\n", c, P4(a), P4(b), P4(d));
		}
	}
	{
		printf("# math/vec4_div\n");
		for (int c = 0; c < CASES; c++) {
			struct vec4 a = rv4(), b = rv4(), d = {0};
			vec4_div(&d, &a, &b);
			printf("case %d: a(%.9g %.9g %.9g %.9g) b(%.9g %.9g %.9g %.9g) -> %.9g %.9g %.9g %.9g\n", c, P4(a), P4(b), P4(d));
		}
	}
	{
		printf("# math/vec4_addf\n");
		for (int c = 0; c < CASES; c++) {
			struct vec4 v = rv4(), d = {0};
			float f = rr();
			vec4_addf(&d, &v, f);
			printf("case %d: v(%.9g %.9g %.9g %.9g) f=%.9g -> %.9g %.9g %.9g %.9g\n", c, P4(v), f, P4(d));
		}
	}
	{
		printf("# math/vec4_subf\n");
		for (int c = 0; c < CASES; c++) {
			struct vec4 v = rv4(), d = {0};
			float f = rr();
			vec4_subf(&d, &v, f);
			printf("case %d: v(%.9g %.9g %.9g %.9g) f=%.9g -> %.9g %.9g %.9g %.9g\n", c, P4(v), f, P4(d));
		}
	}
	{
		printf("# math/vec4_mulf\n");
		for (int c = 0; c < CASES; c++) {
			struct vec4 v = rv4(), d = {0};
			float f = rr();
			vec4_mulf(&d, &v, f);
			printf("case %d: v(%.9g %.9g %.9g %.9g) f=%.9g -> %.9g %.9g %.9g %.9g\n", c, P4(v), f, P4(d));
		}
	}
	{
		printf("# math/vec4_divf\n");
		for (int c = 0; c < CASES; c++) {
			struct vec4 v = rv4(), d = {0};
			float f = rr();
			vec4_divf(&d, &v, f);
			printf("case %d: v(%.9g %.9g %.9g %.9g) f=%.9g -> %.9g %.9g %.9g %.9g\n", c, P4(v), f, P4(d));
		}
	}
	{
		printf("# math/vec4_dot\n");
		for (int c = 0; c < CASES; c++) {
			struct vec4 a = rv4(), b = rv4();
			float r = vec4_dot(&a, &b);
			printf("case %d: a(%.9g %.9g %.9g %.9g) b(%.9g %.9g %.9g %.9g) -> %.9g\n", c, P4(a), P4(b), r);
		}
	}
	{
		printf("# math/vec4_neg\n");
		for (int c = 0; c < CASES; c++) {
			struct vec4 v = rv4(), d = {0};
			vec4_neg(&d, &v);
			printf("case %d: v(%.9g %.9g %.9g %.9g) -> %.9g %.9g %.9g %.9g\n", c, P4(v), P4(d));
		}
	}
	{
		printf("# math/vec4_len\n");
		for (int c = 0; c < CASES; c++) {
			struct vec4 v = rv4();
			float r = vec4_len(&v);
			printf("case %d: v(%.9g %.9g %.9g %.9g) -> %.9g\n", c, P4(v), r);
		}
	}
	{
		printf("# math/vec4_dist\n");
		for (int c = 0; c < CASES; c++) {
			struct vec4 a = rv4(), b = rv4();
			float r = vec4_dist(&a, &b);
			printf("case %d: a(%.9g %.9g %.9g %.9g) b(%.9g %.9g %.9g %.9g) -> %.9g\n", c, P4(a), P4(b), r);
		}
	}
	{
		printf("# math/vec4_norm\n");
		for (int c = 0; c < CASES; c++) {
			struct vec4 v = rv4(), d = {0};
			vec4_norm(&d, &v);
			printf("case %d: v(%.9g %.9g %.9g %.9g) -> %.9g %.9g %.9g %.9g\n", c, P4(v), P4(d));
		}
	}
	{
		printf("# math/vec4_close\n");
		for (int c = 0; c < CASES; c++) {
			struct vec4 a = rv4(), b = rv4();
			float eps = rr();
			int r = vec4_close(&a, &b, eps);
			printf("case %d: a(%.9g %.9g %.9g %.9g) b(%.9g %.9g %.9g %.9g) eps=%.9g -> %d\n", c, P4(a), P4(b), eps, r);
		}
	}
	{
		printf("# math/vec4_min\n");
		for (int c = 0; c < CASES; c++) {
			struct vec4 v = rv4(), minv = rv4(), d = {0};
			vec4_min(&d, &v, &minv);
			printf("case %d: v(%.9g %.9g %.9g %.9g) min(%.9g %.9g %.9g %.9g) -> %.9g %.9g %.9g %.9g\n", c, P4(v), P4(minv), P4(d));
		}
	}
	{
		printf("# math/vec4_minf\n");
		for (int c = 0; c < CASES; c++) {
			struct vec4 v = rv4(), d = {0};
			float f = rr();
			vec4_minf(&d, &v, f);
			printf("case %d: v(%.9g %.9g %.9g %.9g) f=%.9g -> %.9g %.9g %.9g %.9g\n", c, P4(v), f, P4(d));
		}
	}
	{
		printf("# math/vec4_max\n");
		for (int c = 0; c < CASES; c++) {
			struct vec4 v = rv4(), maxv = rv4(), d = {0};
			vec4_max(&d, &v, &maxv);
			printf("case %d: v(%.9g %.9g %.9g %.9g) max(%.9g %.9g %.9g %.9g) -> %.9g %.9g %.9g %.9g\n", c, P4(v), P4(maxv), P4(d));
		}
	}
	{
		printf("# math/vec4_maxf\n");
		for (int c = 0; c < CASES; c++) {
			struct vec4 v = rv4(), d = {0};
			float f = rr();
			vec4_maxf(&d, &v, f);
			printf("case %d: v(%.9g %.9g %.9g %.9g) f=%.9g -> %.9g %.9g %.9g %.9g\n", c, P4(v), f, P4(d));
		}
	}
	{
		printf("# math/vec4_abs\n");
		for (int c = 0; c < CASES; c++) {
			struct vec4 v = rv4(), d = {0};
			vec4_abs(&d, &v);
			printf("case %d: v(%.9g %.9g %.9g %.9g) -> %.9g %.9g %.9g %.9g\n", c, P4(v), P4(d));
		}
	}
	{
		printf("# math/vec4_floor\n");
		for (int c = 0; c < CASES; c++) {
			struct vec4 v = rv4(), d = {0};
			vec4_floor(&d, &v);
			printf("case %d: v(%.9g %.9g %.9g %.9g) -> %.9g %.9g %.9g %.9g\n", c, P4(v), P4(d));
		}
	}
	{
		printf("# math/vec4_ceil\n");
		for (int c = 0; c < CASES; c++) {
			struct vec4 v = rv4(), d = {0};
			vec4_ceil(&d, &v);
			printf("case %d: v(%.9g %.9g %.9g %.9g) -> %.9g %.9g %.9g %.9g\n", c, P4(v), P4(d));
		}
	}
	{
		printf("# math/vec4_from_rgba\n");
		for (int c = 0; c < CASES; c++) {
			uint32_t rgba = next_u32();
			struct vec4 d = {0};
			vec4_from_rgba(&d, rgba);
			printf("case %d: 0x%08x -> %.9g %.9g %.9g %.9g\n", c, rgba, P4(d));
		}
	}
	{
		printf("# math/vec4_from_bgra\n");
		for (int c = 0; c < CASES; c++) {
			uint32_t bgra = next_u32();
			struct vec4 d = {0};
			vec4_from_bgra(&d, bgra);
			printf("case %d: 0x%08x -> %.9g %.9g %.9g %.9g\n", c, bgra, P4(d));
		}
	}
	{
		printf("# math/vec4_from_rgba_srgb\n");
		for (int c = 0; c < CASES; c++) {
			uint32_t rgba = next_u32();
			struct vec4 d = {0};
			vec4_from_rgba_srgb(&d, rgba);
			printf("case %d: 0x%08x -> %.9g %.9g %.9g %.9g\n", c, rgba, P4(d));
		}
	}
	{
		printf("# math/vec4_transform\n");
		for (int c = 0; c < CASES; c++) {
			struct vec4 v = rv4(), d = {0};
			struct matrix4 m = rm4();
			vec4_transform(&d, &v, &m);
			printf("case %d: v(%.9g %.9g %.9g %.9g) m(%s) -> %.9g %.9g %.9g %.9g\n", c, P4(v), pm4(&m), P4(d));
		}
	}

	/* ================= matrix3 ================= */
	{
		printf("# math/matrix3_copy\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix3 m = rm3(), d = {0};
			matrix3_copy(&d, &m);
			printf("case %d: %s -> %s\n", c, pm3(&m), pm3(&d));
		}
	}
	{
		printf("# math/matrix3_identity\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix3 d = {0};
			matrix3_identity(&d);
			printf("case %d: -> %s\n", c, pm3(&d));
		}
	}
	{
		printf("# math/matrix3_from_quat\n");
		for (int c = 0; c < CASES; c++) {
			struct quat q = rq();
			struct matrix3 d = {0};
			matrix3_from_quat(&d, &q);
			printf("case %d: q(%.9g %.9g %.9g %.9g) -> %s\n", c, P4(q), pm3(&d));
		}
	}
	{
		printf("# math/matrix3_from_axisang\n");
		for (int c = 0; c < CASES; c++) {
			struct axisang a = raa();
			struct matrix3 d = {0};
			matrix3_from_axisang(&d, &a);
			printf("case %d: aa(%.9g %.9g %.9g %.9g) -> %s\n", c, P4(a), pm3(&d));
		}
	}
	{
		printf("# math/matrix3_from_matrix4\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix4 m = rm4();
			struct matrix3 d = {0};
			matrix3_from_matrix4(&d, &m);
			printf("case %d: %s -> %s\n", c, pm4(&m), pm3(&d));
		}
	}
	{
		printf("# math/matrix3_mul\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix3 a = rm3(), b = rm3(), d = {0};
			matrix3_mul(&d, &a, &b);
			printf("case %d: a(%s) b(%s) -> %s\n", c, pm3(&a), pm3(&b), pm3(&d));
		}
	}
	{
		printf("# math/matrix3_translate\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix3 m = rm3(), d = {0};
			struct vec3 v = rv3();
			matrix3_translate(&d, &m, &v);
			printf("case %d: m(%s) v(%.9g %.9g %.9g) -> %s\n", c, pm3(&m), P3(v), pm3(&d));
		}
	}
	{
		printf("# math/matrix3_rotate\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix3 m = rm3(), d = {0};
			struct quat q = rq();
			matrix3_rotate(&d, &m, &q);
			printf("case %d: m(%s) q(%.9g %.9g %.9g %.9g) -> %s\n", c, pm3(&m), P4(q), pm3(&d));
		}
	}
	{
		printf("# math/matrix3_rotate_aa\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix3 m = rm3(), d = {0};
			struct axisang a = raa();
			matrix3_rotate_aa(&d, &m, &a);
			printf("case %d: m(%s) aa(%.9g %.9g %.9g %.9g) -> %s\n", c, pm3(&m), P4(a), pm3(&d));
		}
	}
	{
		printf("# math/matrix3_scale\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix3 m = rm3(), d = {0};
			struct vec3 v = rv3();
			matrix3_scale(&d, &m, &v);
			printf("case %d: m(%s) v(%.9g %.9g %.9g) -> %s\n", c, pm3(&m), P3(v), pm3(&d));
		}
	}
	{
		printf("# math/matrix3_transpose\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix3 m = rm3(), d = {0};
			matrix3_transpose(&d, &m);
			printf("case %d: %s -> %s\n", c, pm3(&m), pm3(&d));
		}
	}
	{
		printf("# math/matrix3_inv\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix3 m = rm3(), d = {0};
			matrix3_inv(&d, &m);
			printf("case %d: %s -> %s\n", c, pm3(&m), pm3(&d));
		}
	}
	{
		printf("# math/matrix3_mirror\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix3 m = rm3(), d = {0};
			struct plane p = rpl();
			matrix3_mirror(&d, &m, &p);
			printf("case %d: m(%s) p(dir(%.9g %.9g %.9g) dist=%.9g) -> %s\n", c, pm3(&m), P3(p.dir), p.dist, pm3(&d));
		}
	}
	{
		printf("# math/matrix3_mirrorv\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix3 m = rm3(), d = {0};
			struct vec3 v = rv3();
			matrix3_mirrorv(&d, &m, &v);
			printf("case %d: m(%s) v(%.9g %.9g %.9g) -> %s\n", c, pm3(&m), P3(v), pm3(&d));
		}
	}
	{
		printf("# math/matrix3_translate3f\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix3 m = rm3(), d = {0};
			float x = rr(), y = rr(), z = rr();
			matrix3_translate3f(&d, &m, x, y, z);
			printf("case %d: m(%s) (%.9g %.9g %.9g) -> %s\n", c, pm3(&m), x, y, z, pm3(&d));
		}
	}
	{
		printf("# math/matrix3_rotate_aa4f\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix3 m = rm3(), d = {0};
			float x = rr(), y = rr(), z = rr(), ang = rr() * 3.14159265f;
			matrix3_rotate_aa4f(&d, &m, x, y, z, ang);
			printf("case %d: m(%s) axis(%.9g %.9g %.9g) ang=%.9g -> %s\n", c, pm3(&m), x, y, z, ang, pm3(&d));
		}
	}
	{
		printf("# math/matrix3_scale3f\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix3 m = rm3(), d = {0};
			float x = rr(), y = rr(), z = rr();
			matrix3_scale3f(&d, &m, x, y, z);
			printf("case %d: m(%s) (%.9g %.9g %.9g) -> %s\n", c, pm3(&m), x, y, z, pm3(&d));
		}
	}

	/* ================= matrix4 ================= */
	{
		printf("# math/matrix4_copy\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix4 m = rm4(), d = {0};
			matrix4_copy(&d, &m);
			printf("case %d: %s -> %s\n", c, pm4(&m), pm4(&d));
		}
	}
	{
		printf("# math/matrix4_identity\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix4 d = {0};
			matrix4_identity(&d);
			printf("case %d: -> %s\n", c, pm4(&d));
		}
	}
	{
		printf("# math/matrix4_from_matrix3\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix3 m = rm3();
			struct matrix4 d = {0};
			matrix4_from_matrix3(&d, &m);
			printf("case %d: %s -> %s\n", c, pm3(&m), pm4(&d));
		}
	}
	{
		printf("# math/matrix4_from_quat\n");
		for (int c = 0; c < CASES; c++) {
			struct quat q = rq();
			struct matrix4 d = {0};
			matrix4_from_quat(&d, &q);
			printf("case %d: q(%.9g %.9g %.9g %.9g) -> %s\n", c, P4(q), pm4(&d));
		}
	}
	{
		printf("# math/matrix4_from_axisang\n");
		for (int c = 0; c < CASES; c++) {
			struct axisang a = raa();
			struct matrix4 d = {0};
			matrix4_from_axisang(&d, &a);
			printf("case %d: aa(%.9g %.9g %.9g %.9g) -> %s\n", c, P4(a), pm4(&d));
		}
	}
	{
		printf("# math/matrix4_mul\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix4 a = rm4(), b = rm4(), d = {0};
			matrix4_mul(&d, &a, &b);
			printf("case %d: a(%s) b(%s) -> %s\n", c, pm4(&a), pm4(&b), pm4(&d));
		}
	}
	{
		printf("# math/matrix4_determinant\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix4 m = rm4();
			float r = matrix4_determinant(&m);
			printf("case %d: %s -> %.9g\n", c, pm4(&m), r);
		}
	}
	{
		printf("# math/matrix4_translate3v\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix4 m = rm4(), d = {0};
			struct vec3 v = rv3();
			matrix4_translate3v(&d, &m, &v);
			printf("case %d: m(%s) v(%.9g %.9g %.9g) -> %s\n", c, pm4(&m), P3(v), pm4(&d));
		}
	}
	{
		printf("# math/matrix4_translate4v\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix4 m = rm4(), d = {0};
			struct vec4 v = rv4();
			matrix4_translate4v(&d, &m, &v);
			printf("case %d: m(%s) v(%.9g %.9g %.9g %.9g) -> %s\n", c, pm4(&m), P4(v), pm4(&d));
		}
	}
	{
		printf("# math/matrix4_rotate\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix4 m = rm4(), d = {0};
			struct quat q = rq();
			matrix4_rotate(&d, &m, &q);
			printf("case %d: m(%s) q(%.9g %.9g %.9g %.9g) -> %s\n", c, pm4(&m), P4(q), pm4(&d));
		}
	}
	{
		printf("# math/matrix4_rotate_aa\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix4 m = rm4(), d = {0};
			struct axisang a = raa();
			matrix4_rotate_aa(&d, &m, &a);
			printf("case %d: m(%s) aa(%.9g %.9g %.9g %.9g) -> %s\n", c, pm4(&m), P4(a), pm4(&d));
		}
	}
	{
		printf("# math/matrix4_scale\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix4 m = rm4(), d = {0};
			struct vec3 v = rv3();
			matrix4_scale(&d, &m, &v);
			printf("case %d: m(%s) v(%.9g %.9g %.9g) -> %s\n", c, pm4(&m), P3(v), pm4(&d));
		}
	}
	{
		printf("# math/matrix4_inv\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix4 m = rm4(), d = {0};
			bool ok = matrix4_inv(&d, &m);
			printf("case %d: %s -> %d\n", c, pm4(&m), (int)ok);
			if (ok)
				printf(" result: %s\n", pm4(&d));
		}
	}
	{
		printf("# math/matrix4_transpose\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix4 m = rm4(), d = {0};
			matrix4_transpose(&d, &m);
			printf("case %d: %s -> %s\n", c, pm4(&m), pm4(&d));
		}
	}
	{
		printf("# math/matrix4_translate3v_i\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix4 m = rm4(), d = {0};
			struct vec3 v = rv3();
			matrix4_translate3v_i(&d, &v, &m);
			printf("case %d: v(%.9g %.9g %.9g) m(%s) -> %s\n", c, P3(v), pm4(&m), pm4(&d));
		}
	}
	{
		printf("# math/matrix4_translate4v_i\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix4 m = rm4(), d = {0};
			struct vec4 v = rv4();
			matrix4_translate4v_i(&d, &v, &m);
			printf("case %d: v(%.9g %.9g %.9g %.9g) m(%s) -> %s\n", c, P4(v), pm4(&m), pm4(&d));
		}
	}
	{
		printf("# math/matrix4_rotate_i\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix4 m = rm4(), d = {0};
			struct quat q = rq();
			matrix4_rotate_i(&d, &q, &m);
			printf("case %d: q(%.9g %.9g %.9g %.9g) m(%s) -> %s\n", c, P4(q), pm4(&m), pm4(&d));
		}
	}
	{
		printf("# math/matrix4_rotate_aa_i\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix4 m = rm4(), d = {0};
			struct axisang a = raa();
			matrix4_rotate_aa_i(&d, &a, &m);
			printf("case %d: aa(%.9g %.9g %.9g %.9g) m(%s) -> %s\n", c, P4(a), pm4(&m), pm4(&d));
		}
	}
	{
		printf("# math/matrix4_scale_i\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix4 m = rm4(), d = {0};
			struct vec3 v = rv3();
			matrix4_scale_i(&d, &v, &m);
			printf("case %d: v(%.9g %.9g %.9g) m(%s) -> %s\n", c, P3(v), pm4(&m), pm4(&d));
		}
	}
	{
		printf("# math/matrix4_translate3f\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix4 m = rm4(), d = {0};
			float x = rr(), y = rr(), z = rr();
			matrix4_translate3f(&d, &m, x, y, z);
			printf("case %d: m(%s) (%.9g %.9g %.9g) -> %s\n", c, pm4(&m), x, y, z, pm4(&d));
		}
	}
	{
		printf("# math/matrix4_rotate_aa4f\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix4 m = rm4(), d = {0};
			float x = rr(), y = rr(), z = rr(), ang = rr() * 3.14159265f;
			matrix4_rotate_aa4f(&d, &m, x, y, z, ang);
			printf("case %d: m(%s) axis(%.9g %.9g %.9g) ang=%.9g -> %s\n", c, pm4(&m), x, y, z, ang, pm4(&d));
		}
	}
	{
		printf("# math/matrix4_scale3f\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix4 m = rm4(), d = {0};
			float x = rr(), y = rr(), z = rr();
			matrix4_scale3f(&d, &m, x, y, z);
			printf("case %d: m(%s) (%.9g %.9g %.9g) -> %s\n", c, pm4(&m), x, y, z, pm4(&d));
		}
	}

	/* ================= quat ================= */
	{
		printf("# math/quat_identity\n");
		for (int c = 0; c < CASES; c++) {
			struct quat d = {0};
			quat_identity(&d);
			printf("case %d: -> %.9g %.9g %.9g %.9g\n", c, P4(d));
		}
	}
	{
		printf("# math/quat_set\n");
		for (int c = 0; c < CASES; c++) {
			struct quat d = {0};
			float x = rr(), y = rr(), z = rr(), w = rr();
			quat_set(&d, x, y, z, w);
			printf("case %d: (%.9g %.9g %.9g %.9g) -> %.9g %.9g %.9g %.9g\n", c, x, y, z, w, P4(d));
		}
	}
	{
		printf("# math/quat_copy\n");
		for (int c = 0; c < CASES; c++) {
			struct quat q = rq(), d = {0};
			quat_copy(&d, &q);
			printf("case %d: (%.9g %.9g %.9g %.9g) -> %.9g %.9g %.9g %.9g\n", c, P4(q), P4(d));
		}
	}
	{
		printf("# math/quat_add\n");
		for (int c = 0; c < CASES; c++) {
			struct quat a = rq(), b = rq(), d = {0};
			quat_add(&d, &a, &b);
			printf("case %d: a(%.9g %.9g %.9g %.9g) b(%.9g %.9g %.9g %.9g) -> %.9g %.9g %.9g %.9g\n", c, P4(a), P4(b), P4(d));
		}
	}
	{
		printf("# math/quat_sub\n");
		for (int c = 0; c < CASES; c++) {
			struct quat a = rq(), b = rq(), d = {0};
			quat_sub(&d, &a, &b);
			printf("case %d: a(%.9g %.9g %.9g %.9g) b(%.9g %.9g %.9g %.9g) -> %.9g %.9g %.9g %.9g\n", c, P4(a), P4(b), P4(d));
		}
	}
	{
		printf("# math/quat_mul\n");
		for (int c = 0; c < CASES; c++) {
			struct quat a = rq(), b = rq(), d = {0};
			quat_mul(&d, &a, &b);
			printf("case %d: a(%.9g %.9g %.9g %.9g) b(%.9g %.9g %.9g %.9g) -> %.9g %.9g %.9g %.9g\n", c, P4(a), P4(b), P4(d));
		}
	}
	{
		printf("# math/quat_addf\n");
		for (int c = 0; c < CASES; c++) {
			struct quat q = rq(), d = {0};
			float f = rr();
			quat_addf(&d, &q, f);
			printf("case %d: q(%.9g %.9g %.9g %.9g) f=%.9g -> %.9g %.9g %.9g %.9g\n", c, P4(q), f, P4(d));
		}
	}
	{
		printf("# math/quat_subf\n");
		for (int c = 0; c < CASES; c++) {
			struct quat q = rq(), d = {0};
			float f = rr();
			quat_subf(&d, &q, f);
			printf("case %d: q(%.9g %.9g %.9g %.9g) f=%.9g -> %.9g %.9g %.9g %.9g\n", c, P4(q), f, P4(d));
		}
	}
	{
		printf("# math/quat_mulf\n");
		for (int c = 0; c < CASES; c++) {
			struct quat q = rq(), d = {0};
			float f = rr();
			quat_mulf(&d, &q, f);
			printf("case %d: q(%.9g %.9g %.9g %.9g) f=%.9g -> %.9g %.9g %.9g %.9g\n", c, P4(q), f, P4(d));
		}
	}
	{
		printf("# math/quat_divf\n");
		for (int c = 0; c < CASES; c++) {
			struct quat q = rq(), d = {0};
			float f = rr();
			quat_divf(&d, &q, f);
			printf("case %d: q(%.9g %.9g %.9g %.9g) f=%.9g -> %.9g %.9g %.9g %.9g\n", c, P4(q), f, P4(d));
		}
	}
	{
		printf("# math/quat_dot\n");
		for (int c = 0; c < CASES; c++) {
			struct quat a = rq(), b = rq();
			float r = quat_dot(&a, &b);
			printf("case %d: a(%.9g %.9g %.9g %.9g) b(%.9g %.9g %.9g %.9g) -> %.9g\n", c, P4(a), P4(b), r);
		}
	}
	{
		printf("# math/quat_inv\n");
		for (int c = 0; c < CASES; c++) {
			struct quat q = rq(), d = {0};
			quat_inv(&d, &q);
			printf("case %d: q(%.9g %.9g %.9g %.9g) -> %.9g %.9g %.9g %.9g\n", c, P4(q), P4(d));
		}
	}
	{
		printf("# math/quat_neg\n");
		for (int c = 0; c < CASES; c++) {
			struct quat q = rq(), d = {0};
			quat_neg(&d, &q);
			printf("case %d: q(%.9g %.9g %.9g %.9g) -> %.9g %.9g %.9g %.9g\n", c, P4(q), P4(d));
		}
	}
	{
		printf("# math/quat_len\n");
		for (int c = 0; c < CASES; c++) {
			struct quat q = rq();
			float r = quat_len(&q);
			printf("case %d: q(%.9g %.9g %.9g %.9g) -> %.9g\n", c, P4(q), r);
		}
	}
	{
		printf("# math/quat_dist\n");
		for (int c = 0; c < CASES; c++) {
			struct quat a = rq(), b = rq();
			float r = quat_dist(&a, &b);
			printf("case %d: a(%.9g %.9g %.9g %.9g) b(%.9g %.9g %.9g %.9g) -> %.9g\n", c, P4(a), P4(b), r);
		}
	}
	{
		printf("# math/quat_norm\n");
		for (int c = 0; c < CASES; c++) {
			struct quat q = rq(), d = {0};
			quat_norm(&d, &q);
			printf("case %d: q(%.9g %.9g %.9g %.9g) -> %.9g %.9g %.9g %.9g\n", c, P4(q), P4(d));
		}
	}
	{
		printf("# math/quat_close\n");
		for (int c = 0; c < CASES; c++) {
			struct quat a = rq(), b = rq();
			float eps = rr();
			int r = quat_close(&a, &b, eps);
			printf("case %d: a(%.9g %.9g %.9g %.9g) b(%.9g %.9g %.9g %.9g) eps=%.9g -> %d\n", c, P4(a), P4(b), eps, r);
		}
	}
	{
		printf("# math/quat_from_axisang\n");
		for (int c = 0; c < CASES; c++) {
			struct axisang a = raa();
			struct quat d = {0};
			quat_from_axisang(&d, &a);
			printf("case %d: aa(%.9g %.9g %.9g %.9g) -> %.9g %.9g %.9g %.9g\n", c, P4(a), P4(d));
		}
	}
	{
		printf("# math/quat_from_matrix3\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix3 m = rm3();
			struct quat d = {0};
			quat_from_matrix3(&d, &m);
			printf("case %d: %s -> %.9g %.9g %.9g %.9g\n", c, pm3(&m), P4(d));
		}
	}
	{
		printf("# math/quat_from_matrix4\n");
		for (int c = 0; c < CASES; c++) {
			struct matrix4 m = rm4();
			struct quat d = {0};
			quat_from_matrix4(&d, &m);
			printf("case %d: %s -> %.9g %.9g %.9g %.9g\n", c, pm4(&m), P4(d));
		}
	}
	{
		printf("# math/quat_get_dir\n");
		for (int c = 0; c < CASES; c++) {
			struct quat q = rq();
			struct vec3 d = {0};
			quat_get_dir(&d, &q);
			printf("case %d: q(%.9g %.9g %.9g %.9g) -> %.9g %.9g %.9g\n", c, P4(q), P3(d));
		}
	}
	{
		printf("# math/quat_set_look_dir\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 dir = rv3();
			struct quat d = {0};
			quat_set_look_dir(&d, &dir);
			printf("case %d: dir(%.9g %.9g %.9g) -> %.9g %.9g %.9g %.9g\n", c, P3(dir), P4(d));
		}
	}
	{
		printf("# math/quat_log\n");
		for (int c = 0; c < CASES; c++) {
			struct quat q = rq(), d = {0};
			quat_log(&d, &q);
			printf("case %d: q(%.9g %.9g %.9g %.9g) -> %.9g %.9g %.9g %.9g\n", c, P4(q), P4(d));
		}
	}
	{
		printf("# math/quat_exp\n");
		for (int c = 0; c < CASES; c++) {
			struct quat q = rq(), d = {0};
			quat_exp(&d, &q);
			printf("case %d: q(%.9g %.9g %.9g %.9g) -> %.9g %.9g %.9g %.9g\n", c, P4(q), P4(d));
		}
	}
	{
		printf("# math/quat_interpolate\n");
		for (int c = 0; c < CASES; c++) {
			struct quat a = rq(), b = rq(), d = {0};
			float t = r01();
			quat_interpolate(&d, &a, &b, t);
			printf("case %d: a(%.9g %.9g %.9g %.9g) b(%.9g %.9g %.9g %.9g) t=%.9g -> %.9g %.9g %.9g %.9g\n", c, P4(a), P4(b), t,
				   P4(d));
		}
	}
	{
		printf("# math/quat_get_tangent\n");
		for (int c = 0; c < CASES; c++) {
			struct quat prev = rq(), q = rq(), next = rq(), d = {0};
			quat_get_tangent(&d, &prev, &q, &next);
			printf("case %d: prev(%.9g %.9g %.9g %.9g) q(%.9g %.9g %.9g %.9g) next(%.9g %.9g %.9g %.9g) -> "
				   "%.9g %.9g %.9g %.9g\n",
				   c, P4(prev), P4(q), P4(next), P4(d));
		}
	}
	{
		printf("# math/quat_interpolate_cubic\n");
		for (int c = 0; c < CASES; c++) {
			struct quat q1 = rq(), q2 = rq(), m1 = rq(), m2 = rq(), d = {0};
			float t = r01();
			quat_interpolate_cubic(&d, &q1, &q2, &m1, &m2, t);
			printf("case %d: q1(%.9g %.9g %.9g %.9g) q2(%.9g %.9g %.9g %.9g) m1(%.9g %.9g %.9g %.9g) "
				   "m2(%.9g %.9g %.9g %.9g) t=%.9g -> %.9g %.9g %.9g %.9g\n",
				   c, P4(q1), P4(q2), P4(m1), P4(m2), t, P4(d));
		}
	}

	/* ================= axisang ================= */
	{
		printf("# math/axisang_zero\n");
		for (int c = 0; c < CASES; c++) {
			struct axisang d = {0};
			axisang_zero(&d);
			printf("case %d: -> %.9g %.9g %.9g %.9g\n", c, P4(d));
		}
	}
	{
		printf("# math/axisang_copy\n");
		for (int c = 0; c < CASES; c++) {
			struct axisang a = raa(), d = {0};
			axisang_copy(&d, &a);
			printf("case %d: (%.9g %.9g %.9g %.9g) -> %.9g %.9g %.9g %.9g\n", c, P4(a), P4(d));
		}
	}
	{
		printf("# math/axisang_set\n");
		for (int c = 0; c < CASES; c++) {
			struct axisang d = {0};
			float x = rr(), y = rr(), z = rr(), w = rr();
			axisang_set(&d, x, y, z, w);
			printf("case %d: (%.9g %.9g %.9g %.9g) -> %.9g %.9g %.9g %.9g\n", c, x, y, z, w, P4(d));
		}
	}
	{
		printf("# math/axisang_from_quat\n");
		for (int c = 0; c < CASES; c++) {
			struct quat q = rq();
			struct axisang d = {0};
			axisang_from_quat(&d, &q);
			printf("case %d: q(%.9g %.9g %.9g %.9g) -> %.9g %.9g %.9g %.9g\n", c, P4(q), P4(d));
		}
	}

	/* ================= plane ================= */
	{
		printf("# math/plane_copy\n");
		for (int c = 0; c < CASES; c++) {
			struct plane p = rpl(), d = {0};
			plane_copy(&d, &p);
			printf("case %d: p(dir(%.9g %.9g %.9g) dist=%.9g) -> dir(%.9g %.9g %.9g) dist=%.9g\n", c, P3(p.dir), p.dist, P3(d.dir),
				   d.dist);
		}
	}
	{
		printf("# math/plane_set\n");
		for (int c = 0; c < CASES; c++) {
			struct plane d = {0};
			struct vec3 dir = rv3();
			float dist = rr();
			plane_set(&d, &dir, dist);
			printf("case %d: dir(%.9g %.9g %.9g) dist=%.9g -> dir(%.9g %.9g %.9g) dist=%.9g\n", c, P3(dir), dist, P3(d.dir), d.dist);
		}
	}
	{
		printf("# math/plane_setf\n");
		for (int c = 0; c < CASES; c++) {
			struct plane d = {0};
			float x = rr(), y = rr(), z = rr(), dist = rr();
			plane_setf(&d, x, y, z, dist);
			printf("case %d: (%.9g %.9g %.9g) dist=%.9g -> dir(%.9g %.9g %.9g) dist=%.9g\n", c, x, y, z, dist, P3(d.dir),
				   d.dist);
		}
	}
	{
		printf("# math/plane_from_tri\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 v1 = rv3(), v2 = rv3(), v3 = rv3();
			struct plane d = {0};
			plane_from_tri(&d, &v1, &v2, &v3);
			printf("case %d: v1(%.9g %.9g %.9g) v2(%.9g %.9g %.9g) v3(%.9g %.9g %.9g) -> dir(%.9g %.9g %.9g) dist=%.9g\n", c, P3(v1), P3(v2), P3(v3),
				   P3(d.dir), d.dist);
		}
	}
	{
		printf("# math/plane_transform\n");
		for (int c = 0; c < CASES; c++) {
			struct plane p = rpl(), d = {0};
			struct matrix4 m = rm4();
			plane_transform(&d, &p, &m);
			printf("case %d: p(dir(%.9g %.9g %.9g) dist=%.9g) m(%s) -> dir(%.9g %.9g %.9g) dist=%.9g\n", c, P3(p.dir), p.dist, pm4(&m),
				   P3(d.dir), d.dist);
		}
	}
	{
		printf("# math/plane_transform3x4\n");
		for (int c = 0; c < CASES; c++) {
			struct plane p = rpl(), d = {0};
			struct matrix3 m = rm3();
			plane_transform3x4(&d, &p, &m);
			printf("case %d: p(dir(%.9g %.9g %.9g) dist=%.9g) m(%s) -> dir(%.9g %.9g %.9g) dist=%.9g\n", c, P3(p.dir), p.dist, pm3(&m),
				   P3(d.dir), d.dist);
		}
	}
	{
		printf("# math/plane_intersection_ray\n");
		for (int c = 0; c < CASES; c++) {
			struct plane p = rpl();
			struct vec3 orig = rv3(), dir = rv3();
			float t = 0.0f;
			bool r = plane_intersection_ray(&p, &orig, &dir, &t);
			printf("case %d: p(dir(%.9g %.9g %.9g) dist=%.9g) orig(%.9g %.9g %.9g) dir(%.9g %.9g %.9g) -> %d t=%.9g\n", c, P3(p.dir), p.dist,
				   P3(orig), P3(dir), (int)r, t);
		}
	}
	{
		printf("# math/plane_intersection_line\n");
		for (int c = 0; c < CASES; c++) {
			struct plane p = rpl();
			struct vec3 v1 = rv3(), v2 = rv3();
			float t = 0.0f;
			bool r = plane_intersection_line(&p, &v1, &v2, &t);
			printf("case %d: p(dir(%.9g %.9g %.9g) dist=%.9g) v1(%.9g %.9g %.9g) v2(%.9g %.9g %.9g) -> %d t=%.9g\n", c, P3(p.dir), p.dist, P3(v1),
				   P3(v2), (int)r, t);
		}
	}
	{
		printf("# math/plane_tri_inside\n");
		for (int c = 0; c < CASES; c++) {
			struct plane p = rpl();
			struct vec3 v1 = rv3(), v2 = rv3(), v3 = rv3();
			float eps = rr();
			bool r = plane_tri_inside(&p, &v1, &v2, &v3, eps);
			printf("case %d: p(dir(%.9g %.9g %.9g) dist=%.9g) v1(%.9g %.9g %.9g) v2(%.9g %.9g %.9g) v3(%.9g %.9g %.9g) -> %d\n", c, P3(p.dir), p.dist, P3(v1),
				   P3(v2), P3(v3), (int)r);
		}
	}
	{
		printf("# math/plane_line_inside\n");
		for (int c = 0; c < CASES; c++) {
			struct plane p = rpl();
			struct vec3 v1 = rv3(), v2 = rv3();
			float eps = rr();
			bool r = plane_line_inside(&p, &v1, &v2, eps);
			printf("case %d: p(dir(%.9g %.9g %.9g) dist=%.9g) v1(%.9g %.9g %.9g) v2(%.9g %.9g %.9g) eps=%.9g -> %d\n", c, P3(p.dir), p.dist,
				   P3(v1), P3(v2), eps, (int)r);
		}
	}
	{
		printf("# math/plane_close\n");
		for (int c = 0; c < CASES; c++) {
			struct plane a = rpl(), b = rpl();
			float eps = rr();
			int r = plane_close(&a, &b, eps);
			printf("case %d: a(dir(%.9g %.9g %.9g) dist=%.9g) b(dir(%.9g %.9g %.9g) dist=%.9g) eps=%.9g -> %d\n", c, P3(a.dir), a.dist,
				   P3(b.dir), b.dist, eps, r);
		}
	}
	{
		printf("# math/plane_coplanar\n");
		for (int c = 0; c < CASES; c++) {
			struct plane a = rpl(), b = rpl();
			float eps = rr();
			int r = plane_coplanar(&a, &b, eps);
			printf("case %d: a(dir(%.9g %.9g %.9g) dist=%.9g) b(dir(%.9g %.9g %.9g) dist=%.9g) eps=%.9g -> %d\n", c, P3(a.dir), a.dist,
				   P3(b.dir), b.dist, eps, r);
		}
	}

	/* ================= bounds ================= */
	{
		printf("# math/bounds_zero\n");
		for (int c = 0; c < CASES; c++) {
			struct bounds d = {0};
			bounds_zero(&d);
			printf("case %d: -> min(%.9g %.9g %.9g) max(%.9g %.9g %.9g)\n", c, P3(d.min), P3(d.max));
		}
	}
	{
		printf("# math/bounds_copy\n");
		for (int c = 0; c < CASES; c++) {
			struct bounds b = rbd(), d = {0};
			bounds_copy(&d, &b);
			printf("case %d: min(%.9g %.9g %.9g) max(%.9g %.9g %.9g) -> min(%.9g %.9g %.9g) max(%.9g %.9g %.9g)\n", c, P3(b.min), P3(b.max), P3(d.min),
				   P3(d.max));
		}
	}
	{
		printf("# math/bounds_move\n");
		for (int c = 0; c < CASES; c++) {
			struct bounds b = rbd(), d = {0};
			struct vec3 v = rv3();
			bounds_move(&d, &b, &v);
			printf("case %d: min(%.9g %.9g %.9g) max(%.9g %.9g %.9g) v(%.9g %.9g %.9g) -> min(%.9g %.9g %.9g) max(%.9g %.9g %.9g)\n", c, P3(b.min), P3(b.max), P3(v),
				   P3(d.min), P3(d.max));
		}
	}
	{
		printf("# math/bounds_scale\n");
		for (int c = 0; c < CASES; c++) {
			struct bounds b = rbd(), d = {0};
			struct vec3 v = rv3();
			bounds_scale(&d, &b, &v);
			printf("case %d: min(%.9g %.9g %.9g) max(%.9g %.9g %.9g) v(%.9g %.9g %.9g) -> min(%.9g %.9g %.9g) max(%.9g %.9g %.9g)\n", c, P3(b.min), P3(b.max), P3(v),
				   P3(d.min), P3(d.max));
		}
	}
	{
		printf("# math/bounds_merge\n");
		for (int c = 0; c < CASES; c++) {
			struct bounds a = rbd(), b = rbd(), d = {0};
			bounds_merge(&d, &a, &b);
			printf("case %d: a(min(%.9g %.9g %.9g) max(%.9g %.9g %.9g)) b(min(%.9g %.9g %.9g) max(%.9g %.9g %.9g)) -> min(%.9g %.9g %.9g) max(%.9g %.9g %.9g)\n", c, P3(a.min),
				   P3(a.max), P3(b.min), P3(b.max), P3(d.min), P3(d.max));
		}
	}
	{
		printf("# math/bounds_merge_point\n");
		for (int c = 0; c < CASES; c++) {
			struct bounds b = rbd(), d = {0};
			struct vec3 v = rv3();
			bounds_merge_point(&d, &b, &v);
			printf("case %d: min(%.9g %.9g %.9g) max(%.9g %.9g %.9g) v(%.9g %.9g %.9g) -> min(%.9g %.9g %.9g) max(%.9g %.9g %.9g)\n", c, P3(b.min), P3(b.max), P3(v),
				   P3(d.min), P3(d.max));
		}
	}
	{
		printf("# math/bounds_get_point\n");
		for (int c = 0; c < CASES; c++) {
			struct bounds b = rbd();
			struct vec3 d = {0};
			unsigned int i = next_u32() % 8;
			bounds_get_point(&d, &b, i);
			printf("case %d: min(%.9g %.9g %.9g) max(%.9g %.9g %.9g) i=%u -> %.9g %.9g %.9g\n", c, P3(b.min), P3(b.max), i, P3(d));
		}
	}
	{
		printf("# math/bounds_get_center\n");
		for (int c = 0; c < CASES; c++) {
			struct bounds b = rbd();
			struct vec3 d = {0};
			bounds_get_center(&d, &b);
			printf("case %d: min(%.9g %.9g %.9g) max(%.9g %.9g %.9g) -> %.9g %.9g %.9g\n", c, P3(b.min), P3(b.max), P3(d));
		}
	}
	{
		printf("# math/bounds_transform\n");
		for (int c = 0; c < CASES; c++) {
			struct bounds b = rbd(), d = {0};
			struct matrix4 m = rm4();
			bounds_transform(&d, &b, &m);
			printf("case %d: min(%.9g %.9g %.9g) max(%.9g %.9g %.9g) m(%s) -> min(%.9g %.9g %.9g) max(%.9g %.9g %.9g)\n", c, P3(b.min), P3(b.max), pm4(&m),
				   P3(d.min), P3(d.max));
		}
	}
	{
		printf("# math/bounds_transform3x4\n");
		for (int c = 0; c < CASES; c++) {
			struct bounds b = rbd(), d = {0};
			struct matrix3 m = rm3();
			bounds_transform3x4(&d, &b, &m);
			printf("case %d: min(%.9g %.9g %.9g) max(%.9g %.9g %.9g) m(%s) -> min(%.9g %.9g %.9g) max(%.9g %.9g %.9g)\n", c, P3(b.min), P3(b.max), pm3(&m),
				   P3(d.min), P3(d.max));
		}
	}
	{
		printf("# math/bounds_intersection_ray\n");
		for (int c = 0; c < CASES; c++) {
			struct bounds b = rbd();
			struct vec3 orig = rv3(), dir = rv3();
			float t = 0.0f;
			bool r = bounds_intersection_ray(&b, &orig, &dir, &t);
			printf("case %d: min(%.9g %.9g %.9g) max(%.9g %.9g %.9g) orig(%.9g %.9g %.9g) dir(%.9g %.9g %.9g) -> %d t=%.9g\n", c, P3(b.min), P3(b.max),
				   P3(orig), P3(dir), (int)r, t);
		}
	}
	{
		printf("# math/bounds_intersection_line\n");
		for (int c = 0; c < CASES; c++) {
			struct bounds b = rbd();
			struct vec3 p1 = rv3(), p2 = rv3();
			float t = 0.0f;
			bool r = bounds_intersection_line(&b, &p1, &p2, &t);
			printf("case %d: min(%.9g %.9g %.9g) max(%.9g %.9g %.9g) p1(%.9g %.9g %.9g) p2(%.9g %.9g %.9g) -> %d t=%.9g\n", c, P3(b.min), P3(b.max),
				   P3(p1), P3(p2), (int)r, t);
		}
	}
	{
		printf("# math/bounds_plane_test\n");
		for (int c = 0; c < CASES; c++) {
			struct bounds b = rbd();
			struct plane p = rpl();
			bool r = bounds_plane_test(&b, &p);
			printf("case %d: min(%.9g %.9g %.9g) max(%.9g %.9g %.9g) p(dir(%.9g %.9g %.9g) dist=%.9g) -> %d\n", c, P3(b.min), P3(b.max),
				   P3(p.dir), p.dist, (int)r);
		}
	}
	{
		printf("# math/bounds_under_plane\n");
		for (int c = 0; c < CASES; c++) {
			struct bounds b = rbd();
			struct plane p = rpl();
			bool r = bounds_under_plane(&b, &p);
			printf("case %d: min(%.9g %.9g %.9g) max(%.9g %.9g %.9g) p(dir(%.9g %.9g %.9g) dist=%.9g) -> %d\n", c, P3(b.min), P3(b.max),
				   P3(p.dir), p.dist, (int)r);
		}
	}
	{
		printf("# math/bounds_inside\n");
		for (int c = 0; c < CASES; c++) {
			struct bounds a = rbd(), b = rbd();
			int r = bounds_inside(&a, &b);
			printf("case %d: a(min(%.9g %.9g %.9g) max(%.9g %.9g %.9g)) b(min(%.9g %.9g %.9g) max(%.9g %.9g %.9g)) -> %d\n", c, P3(a.min), P3(a.max),
				   P3(b.min), P3(b.max), r);
		}
	}
	{
		printf("# math/bounds_vec3_inside\n");
		for (int c = 0; c < CASES; c++) {
			struct bounds b = rbd();
			struct vec3 v = rv3();
			int r = bounds_vec3_inside(&b, &v);
			printf("case %d: min(%.9g %.9g %.9g) max(%.9g %.9g %.9g) v(%.9g %.9g %.9g) -> %d\n", c, P3(b.min), P3(b.max), P3(v), r);
		}
	}
	{
		printf("# math/bounds_intersects\n");
		for (int c = 0; c < CASES; c++) {
			struct bounds a = rbd(), b = rbd();
			float eps = rr();
			int r = bounds_intersects(&a, &b, eps);
			printf("case %d: a(min(%.9g %.9g %.9g) max(%.9g %.9g %.9g)) b(min(%.9g %.9g %.9g) max(%.9g %.9g %.9g)) eps=%.9g -> %d\n", c, P3(a.min), P3(a.max),
				   P3(b.min), P3(b.max), eps, r);
		}
	}
	{
		printf("# math/bounds_intersects_obb\n");
		for (int c = 0; c < CASES; c++) {
			struct bounds a = rbd(), b = rbd();
			struct matrix4 m = rm4();
			float eps = rr();
			int r = bounds_intersects_obb(&a, &b, &m, eps);
			printf("case %d: a(min(%.9g %.9g %.9g) max(%.9g %.9g %.9g)) b(min(%.9g %.9g %.9g) max(%.9g %.9g %.9g)) m(%s) eps=%.9g -> %d\n", c, P3(a.min),
				   P3(a.max), P3(b.min), P3(b.max), pm4(&m), eps, r);
		}
	}
	{
		printf("# math/bounds_intersects_obb3x4\n");
		for (int c = 0; c < CASES; c++) {
			struct bounds a = rbd(), b = rbd();
			struct matrix3 m = rm3();
			float eps = rr();
			int r = bounds_intersects_obb3x4(&a, &b, &m, eps);
			printf("case %d: a(min(%.9g %.9g %.9g) max(%.9g %.9g %.9g)) b(min(%.9g %.9g %.9g) max(%.9g %.9g %.9g)) m(%s) eps=%.9g -> %d\n", c, P3(a.min),
				   P3(a.max), P3(b.min), P3(b.max), pm3(&m), eps, r);
		}
	}
	{
		printf("# math/bounds_intersects_ray\n");
		for (int c = 0; c < CASES; c++) {
			struct bounds b = rbd();
			struct vec3 orig = rv3(), dir = rv3();
			int r = bounds_intersects_ray(&b, &orig, &dir);
			printf("case %d: min(%.9g %.9g %.9g) max(%.9g %.9g %.9g) orig(%.9g %.9g %.9g) dir(%.9g %.9g %.9g) -> %d\n", c, P3(b.min), P3(b.max), P3(orig),
				   P3(dir), r);
		}
	}
	{
		printf("# math/bounds_intersects_line\n");
		for (int c = 0; c < CASES; c++) {
			struct bounds b = rbd();
			struct vec3 p1 = rv3(), p2 = rv3();
			int r = bounds_intersects_line(&b, &p1, &p2);
			printf("case %d: min(%.9g %.9g %.9g) max(%.9g %.9g %.9g) p1(%.9g %.9g %.9g) p2(%.9g %.9g %.9g) -> %d\n", c, P3(b.min), P3(b.max), P3(p1),
				   P3(p2), r);
		}
	}
	{
		printf("# math/bounds_min_dist\n");
		for (int c = 0; c < CASES; c++) {
			struct bounds b = rbd();
			struct plane p = rpl();
			float r = bounds_min_dist(&b, &p);
			printf("case %d: min(%.9g %.9g %.9g) max(%.9g %.9g %.9g) p(dir(%.9g %.9g %.9g) dist=%.9g) -> %.9g\n", c, P3(b.min), P3(b.max),
				   P3(p.dir), p.dist, r);
		}
	}

	/* ================= math-extra ================= */
	{
		printf("# math/polar_to_cart\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 v = rv3(), d = {0};
			polar_to_cart(&d, &v);
			printf("case %d: v(%.9g %.9g %.9g) -> %.9g %.9g %.9g\n", c, P3(v), P3(d));
		}
	}
	{
		printf("# math/cart_to_polar\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 v = rv3(), d = {0};
			cart_to_polar(&d, &v);
			printf("case %d: v(%.9g %.9g %.9g) -> %.9g %.9g %.9g\n", c, P3(v), P3(d));
		}
	}
	{
		printf("# math/norm_to_polar\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 v = rv3();
			struct vec2 d = {0};
			norm_to_polar(&d, &v);
			printf("case %d: v(%.9g %.9g %.9g) -> %.9g %.9g\n", c, P3(v), P2(d));
		}
	}
	{
		printf("# math/polar_to_norm\n");
		for (int c = 0; c < CASES; c++) {
			struct vec2 v = rv2();
			struct vec3 d = {0};
			polar_to_norm(&d, &v);
			printf("case %d: v(%.9g %.9g) -> %.9g %.9g %.9g\n", c, P2(v), P3(d));
		}
	}
	{
		printf("# math/calc_torquef\n");
		for (int c = 0; c < CASES; c++) {
			float v1 = rr(), v2 = rr(), torque = rr(), min_adjust = rr(), t = r01();
			float r = calc_torquef(v1, v2, torque, min_adjust, t);
			printf("case %d: (%.9g %.9g %.9g %.9g %.9g) -> %.9g\n", c, v1, v2, torque, min_adjust, t, r);
		}
	}
	{
		printf("# math/calc_torque\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 v1 = rv3(), v2 = rv3(), d = {0};
			float torque = rr(), min_adjust = rr(), t = r01();
			calc_torque(&d, &v1, &v2, torque, min_adjust, t);
			printf("case %d: v1(%.9g %.9g %.9g) v2(%.9g %.9g %.9g) (%.9g %.9g %.9g) -> %.9g %.9g %.9g\n", c, P3(v1), P3(v2), torque, min_adjust,
				   t, P3(d));
		}
	}
	{
		printf("# math/get_percentage\n");
		for (int c = 0; c < CASES; c++) {
			float start = rr(), end = rr(), mid = rr();
			float r = get_percentage(start, end, mid);
			printf("case %d: (%.9g %.9g %.9g) -> %.9g\n", c, start, end, mid, r);
		}
	}
	{
		printf("# math/get_percentagei\n");
		for (int c = 0; c < CASES; c++) {
			int start = (int)(next_u32() % 1000);
			int end = (int)(next_u32() % 1000);
			int mid = (int)(next_u32() % 1000);
			float r = get_percentagei(start, end, mid);
			printf("case %d: (%d %d %d) -> %.9g\n", c, start, end, mid, r);
		}
	}

	/* ================= rand (platform-dependent: libc rand stream) =================
	 * Everything from this line on is EXCLUDED from the CI golden diff:
	 * libc rand() streams differ between glibc/musl/BSD/macOS. */
	printf("# section: rand (platform-dependent: libc rand stream; excluded from CI diff)\n");
	srand(0x0B5);
	{
		printf("# math/rand_float positive\n");
		for (int c = 0; c < CASES; c++)
			printf("case %d: -> %.9g\n", c, rand_float(1));
	}
	{
		printf("# math/rand_float signed\n");
		for (int c = 0; c < CASES; c++)
			printf("case %d: -> %.9g\n", c, rand_float(0));
	}
	{
		printf("# math/vec3_rand positive\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 d = {0};
			vec3_rand(&d, 1);
			printf("case %d: -> %.9g %.9g %.9g\n", c, P3(d));
		}
	}
	{
		printf("# math/vec3_rand signed\n");
		for (int c = 0; c < CASES; c++) {
			struct vec3 d = {0};
			vec3_rand(&d, 0);
			printf("case %d: -> %.9g %.9g %.9g\n", c, P3(d));
		}
	}
}
