/*
 * Conformance harness (area pyscript, spec S-061): STAND-IN for the SWIG-generated runtime header
 * "swig/swigpyrun.h" (normally produced by `swig -python -external-runtime`; SWIG is not installed here).
 * Provides only what shared/obs-scripting/obs-scripting-python*.c use: swig_type_info, SWIG_TypeQuery,
 * SWIG_ConvertPtr, SWIG_NewPointerObj, SWIG_IsOK. Implemented in gen/pyscript_stubs.c with PyCapsule objects.
 * Test tooling, not part of libobs.
 */
#pragma once
#include <Python.h>

typedef struct swig_type_info {
	const char *name;
	int hidden; /* test knob: type "not found" while set */
} swig_type_info;

#define SWIG_OK 0
#define SWIG_ERROR (-1)
#define SWIG_IsOK(r) ((r) >= 0)

swig_type_info *SWIG_TypeQuery(const char *name);
int SWIG_ConvertPtr(PyObject *obj, void **ptr, swig_type_info *type, int flags);
PyObject *SWIG_NewPointerObj(void *ptr, swig_type_info *type, int flags);
