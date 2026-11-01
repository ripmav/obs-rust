/* Conformance harness (area textft2): fake FreeType types (values match the real library's public ABI). */
#pragma once
#include <stdint.h>

typedef int FT_Error;
typedef long FT_Long;
typedef long FT_Pos;
typedef unsigned int FT_UInt;
typedef int32_t FT_Int32;

typedef struct FT_LibraryRec_ *FT_Library;

typedef enum FT_Render_Mode_ { FT_RENDER_MODE_NORMAL = 0, FT_RENDER_MODE_LIGHT = 1, FT_RENDER_MODE_MONO = 2 } FT_Render_Mode;

#define FT_LOAD_DEFAULT 0
#define FT_LOAD_TARGET_MONO ((FT_Int32)((FT_RENDER_MODE_MONO & 15) << 16))

typedef enum FT_Encoding_ { FT_ENCODING_UNICODE = 0x756e6963 } FT_Encoding;

typedef struct FT_Vector_ {
	FT_Pos x, y;
} FT_Vector;

typedef struct FT_Bitmap_ {
	unsigned int rows;
	unsigned int width;
	int pitch;
	unsigned char *buffer;
} FT_Bitmap;

typedef struct FT_GlyphSlotRec_ {
	FT_Vector advance;
	FT_Bitmap bitmap;
	int bitmap_left;
	int bitmap_top;
} FT_GlyphSlotRec, *FT_GlyphSlot;

typedef struct FT_FaceRec_ {
	FT_GlyphSlot glyph;
	void *priv;
} FT_FaceRec, *FT_Face;

typedef struct FT_SfntName_ {
	int dummy;
} FT_SfntName;

FT_Error FT_Init_FreeType(FT_Library *lib);
FT_Error FT_Done_FreeType(FT_Library lib);
FT_Error FT_New_Face(FT_Library lib, const char *path, FT_Long index, FT_Face *face);
FT_Error FT_Done_Face(FT_Face face);
FT_Error FT_Set_Pixel_Sizes(FT_Face face, FT_UInt w, FT_UInt h);
FT_Error FT_Select_Charmap(FT_Face face, FT_Encoding enc);
FT_UInt FT_Get_Char_Index(FT_Face face, unsigned long code);
FT_Error FT_Load_Glyph(FT_Face face, FT_UInt index, FT_Int32 flags);
FT_Error FT_Render_Glyph(FT_GlyphSlot slot, FT_Render_Mode mode);
