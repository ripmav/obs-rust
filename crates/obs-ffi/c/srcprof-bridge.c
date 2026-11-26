// P1-12.3 (S-055 U3): the source profiler's graphics context for the Rust
// bridge (crates/obs-ffi/src/abi_source_profiler.rs).
//
// The C reference enters the context with `obs->video.graphics`
// (source-profiler.c); the layout is computed from the real libobs
// headers here, so the Rust side carries no mirror. A minimal obsconfig.h
// (the same macros as test/conformance/gen/obsconfig.h) is generated into
// OUT_DIR by build.rs — the CMake-generated one is not visible to cargo,
// and the install-path macros do not affect the layout.
//
// The `obs` guard: the C reference assumes a live core; a NULL core is
// UB there, the port skips the (contextually inert) enter/leave instead.
// The conformance runner and production libobs always provide a live
// `obs`, so the counts match the reference exactly.

#include "obs-internal.h"
#include "graphics/graphics.h"

void obs_ffi_srcprof_enter_context(void)
{
	if (!obs)
		return;
	gs_enter_context(obs->video.graphics);
}

void obs_ffi_srcprof_leave_context(void)
{
	gs_leave_context();
}
