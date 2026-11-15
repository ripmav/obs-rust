// Builds the S-001 §4.2 logging C shim into a static library linked into
// the crate, plus the child-process scenario binary used by the unit tests.
fn main() {
    let out_dir = std::env::var("OUT_DIR").unwrap();

    cc::Build::new()
        .file("c/base-logging.c")
        .include("c")
        .warnings(true)
        .compile("obs_ffi_base_logging");

    let bin_name = if cfg!(windows) {
        "log-test-bin.exe"
    } else {
        "log-test-bin"
    };
    let mut build = cc::Build::new();
    build.file("c/log-test-bin.c").include("c").warnings(true);
    let compiler = build.get_compiler();
    let objs = build
        .try_compile_intermediates()
        .expect("compile log-test-bin");
    let bin_path = format!("{out_dir}/{bin_name}");
    let mut link = std::process::Command::new(compiler.path());
    if compiler.is_like_msvc() {
        link.arg(format!("/Fe:{bin_path}"));
        for obj in &objs {
            link.arg(obj);
        }
    } else {
        for obj in &objs {
            link.arg(obj);
        }
        link.arg("-o").arg(&bin_path);
    }
    let status = link.status().expect("link log-test-bin");
    assert!(status.success(), "linking log-test-bin failed");

    println!("cargo:rustc-env=OBS_FFI_LOGTEST={bin_path}");
    println!("cargo:rerun-if-changed=c/base-logging.c");
    println!("cargo:rerun-if-changed=c/base-logging.h");
    println!("cargo:rerun-if-changed=c/log-test-bin.c");
}
