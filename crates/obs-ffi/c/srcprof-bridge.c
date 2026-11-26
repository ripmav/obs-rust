// P1-12.3 (S-055 U3): the source profiler's graphics context for the Rust
// bridge (crates/obs-ffi/src/abi_source_profiler.rs).
//
// The C reference enters the context with `obs->video.graphics`
// (source-profiler.c); the layout is computed from the real libobs
// headers here, so the Rust side carries no mirror. Not compiled by the
// cargo build.rs (libobs is a gitignored symlink, absent on CI) — the
// swap conformance runner compiles it as a runner source (run.sh), and
// the OBS_ENABLE_RUST CMake build compiles it into libobs (the patch
// adds it to libobs/util/); the c-swap test build links the no-op
// stand-ins from the crate's test module instead.
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
	if (!obs)
		return;
	gs_leave_context();
}
