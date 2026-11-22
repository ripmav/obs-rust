/*
 * Exhaustive f16 round-trip dumper (S-009 §5.8, P1-18.2).
 *
 * For every one of the 65536 binary16 bit patterns P: compute the exact
 * f32 value embedded in P (f16_to_f32_bits, pure integer arithmetic —
 * the same algorithm the Rust test uses, see obs-math half tests), feed
 * it through the C half_from_float, and print the re-encoded bits. The
 * committed golden vectors/math2/half_exhaustive.expected holds the
 * blake2b-512 of the full dump plus spot lines.
 *
 * Build from test/conformance (see the corpus-digests note in the
 * README):
 *
 *   cc -std=gnu11 -O2 -Wall -I../../libobs gen/half_dump.c -o half_dump
 *
 * and run: ./half_dump > /tmp/half_dump.txt; b2sum -l 512 /tmp/half_dump.txt
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "graphics/half.h"

/* Exact f16 -> f32 bit embedding (no floating point): the value of P is
 * always exactly representable in binary32. NaN patterns map to the
 * canonical quiet NaN 0x7fc00000 (the half_from_float NaN test only
 * distinguishes NaN from inf, so any NaN works; canonical keeps C and
 * Rust bit-identical). */
static uint32_t f16_to_f32_bits(uint16_t u)
{
	uint32_t sign = (uint32_t)(u & 0x8000) << 16;
	uint32_t e = (u >> 10) & 0x1F;
	uint32_t m = u & 0x3FF;
	if ((u & 0x7C00) == 0x7C00)
		return 0x7F800000 | ((m ? 0x00400000 : 0) | sign);
	if (e == 0) {
		if (m == 0)
			return sign;
		/* subnormal: value = m * 2^-24; normalise m to a 23-bit
		 * significand (leading bit set) */
		uint32_t f = m;
		while ((f & 0x200) == 0)
			f <<= 1;
		uint32_t bits_m = 32 - __builtin_clz(m); /* m's bit length */
		uint32_t mant = (f & 0x1FF) << 14;
		uint32_t biased = 127 + bits_m - 25;
		return sign | (biased << 23) | mant;
	}
	/* normal: value = (1 + m*2^-10) * 2^(e-15) */
	return sign | ((e + 112) << 23) | (m << 13);
}

int main(void)
{
	for (uint32_t p = 0; p < 0x10000; p++) {
		uint16_t in = (uint16_t)p;
		union {
			uint32_t b;
			float f;
		} x;
		x.b = f16_to_f32_bits(in);
		struct half h = half_from_float(x.f);
		printf("half 0x%04x 0x%04x\n", in, h.u);
	}
	return 0;
}
