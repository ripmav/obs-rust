/*
 * Conformance harness (area textft2, spec S-038): minimal stand-in for FreeType's public types. Only what
 * plugins/text-freetype2 needs is declared; the functions are faked in textft2_stubs.c with scripted glyph
 * metrics, so no FreeType library or font file is needed. Test tooling, not part of libobs.
 */
#pragma once
#include <stdint.h>

#define FT_FREETYPE_H "textft2_ft.h"
#define FT_SFNT_NAMES_H "textft2_ft.h"
#define FT_TRUETYPE_IDS_H "textft2_ft.h"
