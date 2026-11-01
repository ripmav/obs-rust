/* Conformance harness (area textft2, spec S-038): control interface of the fake FreeType / graphics / host layer. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <obs-module.h>

/* scripted font */
void fake_ft_reset(void);
void fake_ft_glyph(unsigned long cp, unsigned index, int w, int h, int left, int top, long adv26_6);
void fake_ft_notdef(int w, int h, int left, int top, long adv26_6);
void fake_ft_pitch_pad(int pad);
void fake_ft_fail_init(bool fail);
void fake_ft_fail_face(bool fail);
void fake_font_table(const char *name, const char *style, uint32_t flags, const char *path);
void fake_gfx_fail_effect(bool fail);
void fake_gfx_linear(bool v);

/* matrix state: current translation */
void fake_clock_set(uint64_t ns);
uint64_t fake_clock_get(void);
extern uint64_t fake_clock_ns;
extern bool fake_events; /* print `ev` lines of the fake FreeType/graphics layer */
