// P1-02.4 (WP-32): compiles the pinned C `libobs/util/dstr.c` plus
// `utf8.c` (the case ops drive the real UTF-8 ↔ wide path) and the shim
// that provides the bmem/base symbols they link against, into the fuzz
// harness, so `dstr_ops` can run the same random op sequence on the Rust
// and the C implementation and compare them.
fn main() {
    let libobs = "../libobs";
    cc::Build::new()
        .file("c/dstr-shim.c")
        .file(format!("{libobs}/util/dstr.c"))
        .file(format!("{libobs}/util/utf8.c"))
        .include("c")
        .include(libobs)
        .flag_if_supported("-std=gnu11")
        .warnings(false) // C reference code; linted by its own build
        .compile("obs_fuzz_c_dstr");

    println!("cargo:rerun-if-changed=c/dstr-shim.c");
    println!("cargo:rerun-if-changed={libobs}/util/dstr.c");
    println!("cargo:rerun-if-changed={libobs}/util/dstr.h");
    println!("cargo:rerun-if-changed={libobs}/util/utf8.c");
}
