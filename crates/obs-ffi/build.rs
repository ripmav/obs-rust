// Builds the S-001 §4.2 logging C shim (plus its test-support handlers)
// and the platform sock-error accessor (the C errno macro resolves the
// per-ABI errno symbol) into the static library linked into the crate.
// Scenario tests re-exec the test binary itself (bmem_fatal pattern), so
// no separate binary is linked here.
fn main() {
    cc::Build::new()
        .file("c/base-logging.c")
        .file("c/logging-test-helpers.c")
        .file("c/sock-error.c")
        .include("c")
        .warnings(true)
        .compile("obs_ffi_base_logging");

    // P1-12.3 (S-055 U3): the source profiler's graphics-context shim.
    // It includes the real libobs headers (the `obs->video.graphics`
    // layout is computed in C, no Rust mirror); the build-generated
    // obsconfig.h / obsversion.h are replaced by minimal stand-ins in
    // OUT_DIR (the CMake-generated ones are not visible to cargo, and
    // only layout-relevant types are used — the version constants and
    // install paths are not). `<caption/caption.h>` resolves against the
    // vendored deps/libcaption (the same layout as the obs-studio tree).
    let out_dir = std::env::var("OUT_DIR").expect("OUT_DIR is set by cargo");
    std::fs::write(
        std::path::Path::new(&out_dir).join("obsconfig.h"),
        "#pragma once\n\
         #define OBS_DATA_PATH \"data/libobs\"\n\
         #define OBS_INSTALL_PREFIX \"\"\n\
         #define OBS_PLUGIN_PATH \"obs-plugins\"\n\
         #define OBS_PLUGIN_DESTINATION \"obs-plugins\"\n",
    )
    .expect("write obsconfig.h");
    std::fs::write(
        std::path::Path::new(&out_dir).join("obsversion.h"),
        "#pragma once\n\
         #define OBS_VERSION \"0.0.0\"\n\
         #define OBS_COMMIT \"none\"\n\
         #define OBS_BRANCH \"none\"\n",
    )
    .expect("write obsversion.h");
    cc::Build::new()
        .file("c/srcprof-bridge.c")
        .include("c")
        .include("../../libobs")
        .include("../../deps/libcaption")
        .include(&out_dir)
        // obs-internal.h → serializer.h (upstream) trips
        // -Wstrict-aliasing in `s_wlf`; the CMake build of libobs does not
        // enable that warning for this translation unit either.
        .flag_if_supported("-Wno-strict-aliasing")
        .warnings(true)
        .compile("obs_ffi_srcprof_bridge");

    println!("cargo:rerun-if-changed=c/base-logging.c");
    println!("cargo:rerun-if-changed=c/base-logging.h");
    println!("cargo:rerun-if-changed=c/logging-test-helpers.c");
    println!("cargo:rerun-if-changed=c/sock-error.c");
    println!("cargo:rerun-if-changed=c/srcprof-bridge.c");
    println!("cargo:rerun-if-changed=../../libobs/obs-internal.h");
    println!("cargo:rerun-if-changed=../../libobs/obs.h");
    println!("cargo:rerun-if-changed=../../libobs/graphics/graphics.h");
}
