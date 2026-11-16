// Builds the S-001 §4.2 logging C shim (plus its test-support handlers)
// into the static library linked into the crate. Scenario tests re-exec
// the test binary itself (bmem_fatal pattern), so no separate binary is
// linked here.
fn main() {
    cc::Build::new()
        .file("c/base-logging.c")
        .file("c/logging-test-helpers.c")
        .include("c")
        .warnings(true)
        .compile("obs_ffi_base_logging");

    println!("cargo:rerun-if-changed=c/base-logging.c");
    println!("cargo:rerun-if-changed=c/base-logging.h");
    println!("cargo:rerun-if-changed=c/logging-test-helpers.c");
}
