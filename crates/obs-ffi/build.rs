// Builds the S-001 §4.2 logging C shim (plus its test-support handlers)
// and the platform sock-error accessor (the C errno macro resolves the
// per-ABI errno symbol) into the static library linked into the crate.
// Scenario tests re-exec the test binary itself (bmem_fatal pattern), so
// no separate binary is linked here.
//
// Note (P1-12.3): the source profiler's graphics-context shim
// (`c/srcprof-bridge.c`) is deliberately NOT compiled here — it includes
// the real libobs headers, which only exist where a pinned obs-studio
// checkout is linked in (the conformance job / dev machines). The swap
// conformance runner compiles it as a runner source (test/conformance/
// run.sh), and the OBS_ENABLE_RUST CMake build compiles it into libobs
// (the patch adds it to `libobs/util/`).
fn main() {
    cc::Build::new()
        .file("c/base-logging.c")
        .file("c/logging-test-helpers.c")
        .file("c/sock-error.c")
        .include("c")
        .warnings(true)
        .compile("obs_ffi_base_logging");

    println!("cargo:rerun-if-changed=c/base-logging.c");
    println!("cargo:rerun-if-changed=c/base-logging.h");
    println!("cargo:rerun-if-changed=c/logging-test-helpers.c");
    println!("cargo:rerun-if-changed=c/sock-error.c");
}
