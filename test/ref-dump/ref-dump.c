/*
 * obs-ref-dump (WP-39): reference dumper for the pinned libobs 33.0.1 C
 * implementation. It links the real C sources (see build.sh) so its output is
 * the authoritative reference the Rust rewrite is conformance-tested against.
 *
 * Skeleton stage: PRNG + --list only. The per-area dumps (math, data, media)
 * come in the follow-up steps; until then every area name reports
 * "not implemented yet".
 */
#include <stdint.h>
#include <stdio.h>
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

/* splitmix64, seed 0x0B5: identical stream on every build, so dumps are
 * comparable across runs and machines. */
static uint64_t sm_state = 0x0B5ULL;

static uint64_t next_u64(void)
{
	uint64_t z = (sm_state += 0x9E3779B97F4A7C15ULL);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}

uint32_t next_u32(void)
{
	return (uint32_t)next_u64();
}

/* uniform float in [0, 1): top 24 bits over 2^24 */
float next_f32(void)
{
	return (float)(next_u32() >> 8) * (1.0f / 16777216.0f);
}

int main(int argc, char *argv[])
{
	if (argc == 2 && strcmp(argv[1], "--list") == 0) {
		printf("math\n");
		printf("data\n");
		printf("media\n");
		return 0;
	}

	if (argc == 2) {
		printf("not implemented yet: %s\n", argv[1]);
		return 1;
	}

	fprintf(stderr, "usage: obs-ref-dump [--list | <area>]\n");
	return 1;
}
