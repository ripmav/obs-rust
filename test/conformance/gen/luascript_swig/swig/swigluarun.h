/*
 * Conformance harness (area luascript, spec S-060): STAND-IN for the SWIG-generated runtime header
 * "swig/swigluarun.h" (normally produced by `swig -lua -external-runtime`; SWIG is not installed here).
 * Provides only what shared/obs-scripting/obs-scripting-lua*.c use: lua_rawlen on LuaJIT, swig_type_info,
 * SWIG_TypeQuery, SWIG_ConvertPtr, SWIG_NewPointerObj, SWIG_IsOK. Implemented in gen/luascript_stubs.c.
 * Test tooling, not part of libobs.
 */
#pragma once
#include <lua.h>

#define lua_rawlen lua_objlen

typedef struct swig_type_info {
	const char *name;
	int hidden; /* test knob: type "not found" while set */
} swig_type_info;

#define SWIG_OK 0
#define SWIG_ERROR (-1)
#define SWIG_IsOK(r) ((r) >= 0)

swig_type_info *SWIG_TypeQuery(lua_State *L, const char *name);
int SWIG_ConvertPtr(lua_State *L, int idx, void *ptr, swig_type_info *type, int flags);
void SWIG_NewPointerObj(lua_State *L, void *ptr, swig_type_info *type, int own);
