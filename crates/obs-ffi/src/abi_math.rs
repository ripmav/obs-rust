//! P1-15.4 / P1-16.5 / P1-17.4 / P1-18.4 recipe step 2 (WP-43): the C ABI
//! for the graphics math files — fully ported to [`obs_math`].
//!
//! With `OBS_ENABLE_RUST=ON` the nine C files (`graphics/{vec3,vec4,quat,
//! axisang,matrix3,matrix4,plane,bounds,math-extra}.c`) are excluded from
//! the build (`test/conformance/obs-enable-rust.patch`); these exports
//! provide their 80 exported symbols (plus the internal
//! `matrix4_mul_4x3_only`) to the rest of libobs and to the conformance
//! runners. `graphics/vec2.c` is **not** swapped: every `vec2` helper is a
//! `static inline` in `vec2.h` (no exports to bridge).
//!
//! The obs_math types are `repr(C)` mirrors of the C structs (layout
//! pinned by the obs-math layout tests), so the C pointers are re-pointed
//! at the Rust types. The C math functions are all alias-safe (the C
//! code computes through temporaries / component-order writes), so the
//! bridge reads the inputs, computes the owned result, and writes it
//! back — never holding a `&mut` and a `&` on overlapping memory.
//! `matrix3_inv` returns `void` in C (the Rust `bool` is dropped); the
//! `matrix4_*` `bool` results are kept.
//!
//! `matrix4_mul_4x3_only` has no declaration in any C header (it is
//! internal to the swapped `matrix4.c`), but the conformance
//! `math_runner.c` calls it — so it is bridged too and excluded from the
//! cbindgen header (the `bitstream_reader_read_bit` precedent).

use std::os::raw::{c_int, c_uint};

use obs_math::axisang::AxisAng;
use obs_math::bounds::Bounds;
use obs_math::matrix3::Matrix3;
use obs_math::matrix4::Matrix4;
use obs_math::plane::Plane;
use obs_math::quat::Quat;
use obs_math::vec2::Vec2;
use obs_math::vec3::Vec3;
use obs_math::vec4::Vec4;

// ── vec3.c (8) ──────────────────────────────────────────────────────────

/// C `vec3_from_vec4(dst, v)` (S-008-R06).
///
/// # Safety
/// C-ABI — `dst`/`v` are valid for the call; the obs_math types mirror
/// the C layouts.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vec3_from_vec4(dst: *mut Vec3, v: *const Vec4) {
    // SAFETY: read-compute-write — no overlapping references (module note).
    unsafe {
        *dst = obs_math::vec3::vec3_from_vec4(&*v);
    }
}

/// C `vec3_plane_dist(v, p)` (S-009-R20).
///
/// # Safety
/// C-ABI — `v`/`p` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vec3_plane_dist(v: *const Vec3, p: *const Plane) -> f32 {
    // SAFETY: read-only (module note).
    unsafe { obs_math::vec3::vec3_plane_dist(&*v, &*p) }
}

/// C `vec3_transform(dst, v, m)` (S-008-R07).
///
/// # Safety
/// C-ABI — `dst`/`v`/`m` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vec3_transform(dst: *mut Vec3, v: *const Vec3, m: *const Matrix4) {
    // SAFETY: read-compute-write (module note).
    unsafe {
        *dst = obs_math::vec3::vec3_transform(&*v, &*m);
    }
}

/// C `vec3_rotate(dst, v, m)` (S-009-R21).
///
/// # Safety
/// C-ABI — `dst`/`v`/`m` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vec3_rotate(dst: *mut Vec3, v: *const Vec3, m: *const Matrix3) {
    // SAFETY: read-compute-write (module note).
    unsafe {
        *dst = obs_math::vec3::vec3_rotate(&*v, &*m);
    }
}

/// C `vec3_transform3x4(dst, v, m)` (S-009-R22).
///
/// # Safety
/// C-ABI — `dst`/`v`/`m` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vec3_transform3x4(dst: *mut Vec3, v: *const Vec3, m: *const Matrix3) {
    // SAFETY: read-compute-write (module note).
    unsafe {
        *dst = obs_math::vec3::vec3_transform3x4(&*v, &*m);
    }
}

/// C `vec3_mirror(dst, v, p)` (S-009-R23).
///
/// # Safety
/// C-ABI — `dst`/`v`/`p` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vec3_mirror(dst: *mut Vec3, v: *const Vec3, p: *const Plane) {
    // SAFETY: read-compute-write (module note).
    unsafe {
        *dst = obs_math::vec3::vec3_mirror(&*v, &*p);
    }
}

/// C `vec3_mirrorv(dst, v, n)` (S-009-R24).
///
/// # Safety
/// C-ABI — `dst`/`v`/`n` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vec3_mirrorv(dst: *mut Vec3, v: *const Vec3, n: *const Vec3) {
    // SAFETY: read-compute-write (module note).
    unsafe {
        *dst = obs_math::vec3::vec3_mirrorv(&*v, &*n);
    }
}

/// C `vec3_rand(dst, positive_only)` (S-009-R25).
///
/// # Safety
/// C-ABI — `dst` is valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vec3_rand(dst: *mut Vec3, positive_only: c_int) {
    // SAFETY: read-compute-write (module note).
    unsafe {
        *dst = obs_math::vec3::vec3_rand(positive_only != 0);
    }
}

// ── vec4.c (2) ──────────────────────────────────────────────────────────

/// C `vec4_from_vec3(dst, v)` (S-008-R27).
///
/// # Safety
/// C-ABI — `dst`/`v` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vec4_from_vec3(dst: *mut Vec4, v: *const Vec3) {
    // SAFETY: read-compute-write (module note).
    unsafe {
        *dst = obs_math::vec4::vec4_from_vec3(&*v);
    }
}

/// C `vec4_transform(dst, v, m)` (S-008-R31, no perspective divide).
///
/// # Safety
/// C-ABI — `dst`/`v`/`m` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vec4_transform(dst: *mut Vec4, v: *const Vec4, m: *const Matrix4) {
    // SAFETY: read-compute-write (module note).
    unsafe {
        *dst = obs_math::vec4::vec4_transform(&*v, &*m);
    }
}

// ── quat.c (11) ─────────────────────────────────────────────────────────

/// C `quat_mul(d, q1, q2)` (S-008-R13).
///
/// # Safety
/// C-ABI — `d`/`q1`/`q2` are valid for the call (the C implementation is
/// alias-safe; the locals below keep the references from overlapping).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn quat_mul(d: *mut Quat, q1: *const Quat, q2: *const Quat) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let q1 = *q1;
        let q2 = *q2;
        let mut val = *d;
        obs_math::quat::quat_mul(&mut val, &q1, &q2);
        *d = val;
    }
}

/// C `quat_from_axisang(d, aa)` (S-008-R14).
///
/// # Safety
/// C-ABI — `d`/`aa` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn quat_from_axisang(d: *mut Quat, aa: *const AxisAng) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let mut val = *d;
        obs_math::quat::quat_from_axisang(&mut val, &*aa);
        *d = val;
    }
}

/// C `quat_from_matrix3(d, m)` (S-008-R16, the 3×4 form).
///
/// # Safety
/// C-ABI — `d`/`m` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn quat_from_matrix3(d: *mut Quat, m: *const Matrix3) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let mut val = *d;
        obs_math::matrix3::quat_from_matrix3(&mut val, &*m);
        *d = val;
    }
}

/// C `quat_from_matrix4(d, m)` (S-008-R16).
///
/// # Safety
/// C-ABI — `d`/`m` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn quat_from_matrix4(d: *mut Quat, m: *const Matrix4) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let mut val = *d;
        obs_math::quat::quat_from_matrix4(&mut val, &*m);
        *d = val;
    }
}

/// C `quat_get_dir(d, q)` (S-008-R17).
///
/// # Safety
/// C-ABI — `d`/`q` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn quat_get_dir(d: *mut Vec3, q: *const Quat) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let mut val = *d;
        obs_math::quat::quat_get_dir(&mut val, &*q);
        *d = val;
    }
}

/// C `quat_set_look_dir(d, dir)` (S-008-R18, Q-3).
///
/// # Safety
/// C-ABI — `d`/`dir` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn quat_set_look_dir(d: *mut Quat, dir: *const Vec3) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let mut val = *d;
        obs_math::quat::quat_set_look_dir(&mut val, &*dir);
        *d = val;
    }
}

/// C `quat_log(d, q)` (S-008-R19).
///
/// # Safety
/// C-ABI — `d`/`q` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn quat_log(d: *mut Quat, q: *const Quat) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let mut val = *d;
        obs_math::quat::quat_log(&mut val, &*q);
        *d = val;
    }
}

/// C `quat_exp(d, q)` (S-008-R19).
///
/// # Safety
/// C-ABI — `d`/`q` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn quat_exp(d: *mut Quat, q: *const Quat) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let mut val = *d;
        obs_math::quat::quat_exp(&mut val, &*q);
        *d = val;
    }
}

/// C `quat_interpolate(d, q1, q2, t)` (S-008-R20, Q-4).
///
/// # Safety
/// C-ABI — `d`/`q1`/`q2` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn quat_interpolate(d: *mut Quat, q1: *const Quat, q2: *const Quat, t: f32) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let q1 = *q1;
        let q2 = *q2;
        let mut val = *d;
        obs_math::quat::quat_interpolate(&mut val, &q1, &q2, t);
        *d = val;
    }
}

/// C `quat_get_tangent(d, prev, q, next)` (S-008-R21).
///
/// # Safety
/// C-ABI — `d`/`prev`/`q`/`next` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn quat_get_tangent(
    d: *mut Quat,
    prev: *const Quat,
    q: *const Quat,
    next: *const Quat,
) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let prev = *prev;
        let q = *q;
        let next = *next;
        let mut val = *d;
        obs_math::quat::quat_get_tangent(&mut val, &prev, &q, &next);
        *d = val;
    }
}

/// C `quat_interpolate_cubic(d, q1, q2, m1, m2, t)` (S-008-R21).
///
/// # Safety
/// C-ABI — all pointers valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn quat_interpolate_cubic(
    d: *mut Quat,
    q1: *const Quat,
    q2: *const Quat,
    m1: *const Quat,
    m2: *const Quat,
    t: f32,
) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let q1 = *q1;
        let q2 = *q2;
        let m1 = *m1;
        let m2 = *m2;
        let mut val = *d;
        obs_math::quat::quat_interpolate_cubic(&mut val, &q1, &q2, &m1, &m2, t);
        *d = val;
    }
}

// ── axisang.c (1) ───────────────────────────────────────────────────────

/// C `axisang_from_quat(d, q)` (S-008-R15).
///
/// # Safety
/// C-ABI — `d`/`q` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn axisang_from_quat(d: *mut AxisAng, q: *const Quat) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let mut val = *d;
        obs_math::axisang::axisang_from_quat(&mut val, &*q);
        *d = val;
    }
}

// ── matrix3.c (11) ──────────────────────────────────────────────────────

/// C `matrix3_from_quat(d, q)` (S-009-R31).
///
/// # Safety
/// C-ABI — `d`/`q` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn matrix3_from_quat(d: *mut Matrix3, q: *const Quat) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let mut val = *d;
        obs_math::matrix3::matrix3_from_quat(&mut val, &*q);
        *d = val;
    }
}

/// C `matrix3_from_axisang(d, aa)` (S-009-R32).
///
/// # Safety
/// C-ABI — `d`/`aa` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn matrix3_from_axisang(d: *mut Matrix3, aa: *const AxisAng) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let mut val = *d;
        obs_math::matrix3::matrix3_from_axisang(&mut val, &*aa);
        *d = val;
    }
}

/// C `matrix3_from_matrix4(d, m)` (S-009-R34).
///
/// # Safety
/// C-ABI — `d`/`m` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn matrix3_from_matrix4(d: *mut Matrix3, m: *const Matrix4) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let mut val = *d;
        obs_math::matrix3::matrix3_from_matrix4(&mut val, &*m);
        *d = val;
    }
}

/// C `matrix3_mul(d, a, b)` (S-009-R35).
///
/// # Safety
/// C-ABI — `d`/`a`/`b` are valid for the call (alias-safe: the port
/// computes through temporaries; the locals keep references disjoint).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn matrix3_mul(d: *mut Matrix3, a: *const Matrix3, b: *const Matrix3) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let a = *a;
        let b = *b;
        let mut val = *d;
        obs_math::matrix3::matrix3_mul(&mut val, &a, &b);
        *d = val;
    }
}

/// C `matrix3_rotate(d, m, q)` (S-009-R37).
///
/// # Safety
/// C-ABI — `d`/`m`/`q` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn matrix3_rotate(d: *mut Matrix3, m: *const Matrix3, q: *const Quat) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let m = *m;
        let mut val = *d;
        obs_math::matrix3::matrix3_rotate(&mut val, &m, &*q);
        *d = val;
    }
}

/// C `matrix3_rotate_aa(d, m, aa)` (S-009-R37).
///
/// # Safety
/// C-ABI — `d`/`m`/`aa` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn matrix3_rotate_aa(d: *mut Matrix3, m: *const Matrix3, aa: *const AxisAng) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let m = *m;
        let mut val = *d;
        obs_math::matrix3::matrix3_rotate_aa(&mut val, &m, &*aa);
        *d = val;
    }
}

/// C `matrix3_scale(d, m, v)` (S-009-R38).
///
/// # Safety
/// C-ABI — `d`/`m`/`v` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn matrix3_scale(d: *mut Matrix3, m: *const Matrix3, v: *const Vec3) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let m = *m;
        let mut val = *d;
        obs_math::matrix3::matrix3_scale(&mut val, &m, &*v);
        *d = val;
    }
}

/// C `matrix3_transpose(d, m)` (S-009-R39, the `w`-lane quirk kept).
///
/// # Safety
/// C-ABI — `d`/`m` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn matrix3_transpose(d: *mut Matrix3, m: *const Matrix3) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let mut val = *d;
        obs_math::matrix3::matrix3_transpose(&mut val, &*m);
        *d = val;
    }
}

/// C `matrix3_inv(d, m)` (S-009-R40) — C returns **void** (the
/// singular-case flag of `matrix4_inv` is discarded; the port keeps the
/// `bool` internally).
///
/// # Safety
/// C-ABI — `d`/`m` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn matrix3_inv(d: *mut Matrix3, m: *const Matrix3) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let mut val = *d;
        // SAFETY: `m` is valid for the call (C-ABI).
        let _ = obs_math::matrix3::matrix3_inv(&mut val, &*m);
        *d = val;
    }
}

/// C `matrix3_mirror(d, m, p)` (S-009-R41).
///
/// # Safety
/// C-ABI — `d`/`m`/`p` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn matrix3_mirror(d: *mut Matrix3, m: *const Matrix3, p: *const Plane) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let m = *m;
        let mut val = *d;
        obs_math::matrix3::matrix3_mirror(&mut val, &m, &*p);
        *d = val;
    }
}

/// C `matrix3_mirrorv(d, m, n)` (S-009-R41).
///
/// # Safety
/// C-ABI — `d`/`m`/`n` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn matrix3_mirrorv(d: *mut Matrix3, m: *const Matrix3, n: *const Vec3) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let m = *m;
        let mut val = *d;
        obs_math::matrix3::matrix3_mirrorv(&mut val, &m, &*n);
        *d = val;
    }
}

// ── matrix4.c (18) ────────────────────────────────────────────────────────

/// C `matrix4_from_matrix3(d, m)` (S-009-R42).
///
/// # Safety
/// C-ABI — `d`/`m` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn matrix4_from_matrix3(d: *mut Matrix4, m: *const Matrix3) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let mut val = *d;
        obs_math::matrix4::matrix4_from_matrix3(&mut val, &*m);
        *d = val;
    }
}

/// C `matrix4_from_quat(d, q)` (S-009-R43).
///
/// # Safety
/// C-ABI — `d`/`q` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn matrix4_from_quat(d: *mut Matrix4, q: *const Quat) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let mut val = *d;
        obs_math::matrix4::matrix4_from_quat(&mut val, &*q);
        *d = val;
    }
}

/// C `matrix4_from_axisang(d, aa)` (S-009-R44).
///
/// # Safety
/// C-ABI — `d`/`aa` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn matrix4_from_axisang(d: *mut Matrix4, aa: *const AxisAng) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let mut val = *d;
        obs_math::matrix4::matrix4_from_axisang(&mut val, &*aa);
        *d = val;
    }
}

/// C `matrix4_mul(d, m1, m2)` (S-009-R45).
///
/// # Safety
/// C-ABI — `d`/`m1`/`m2` are valid for the call (alias-safe).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn matrix4_mul(d: *mut Matrix4, m1: *const Matrix4, m2: *const Matrix4) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let m1 = *m1;
        let m2 = *m2;
        let mut val = *d;
        obs_math::matrix4::matrix4_mul(&mut val, &m1, &m2);
        *d = val;
    }
}

/// C `matrix4_mul_4x3_only(d, m1, m2)` — internal to the C
/// `matrix4.c` (declared in **no** C header; the conformance
/// `math_runner.c` declares it locally, the `bitstream_reader_read_bit`
/// precedent). Excluded from the cbindgen header (cbindgen.toml).
///
/// # Safety
/// C-ABI — `d`/`m1`/`m2` are valid for the call (alias-safe).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn matrix4_mul_4x3_only(
    d: *mut Matrix4,
    m1: *const Matrix4,
    m2: *const Matrix4,
) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let m1 = *m1;
        let m2 = *m2;
        let mut val = *d;
        obs_math::matrix4::matrix4_mul_4x3_only(&mut val, &m1, &m2);
        *d = val;
    }
}

/// C `matrix4_determinant(m)` (S-009-R46).
///
/// # Safety
/// C-ABI — `m` is valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn matrix4_determinant(m: *const Matrix4) -> f32 {
    // SAFETY: read-only (module note).
    unsafe { obs_math::matrix4::matrix4_determinant(&*m) }
}

/// C `matrix4_translate3v(d, m, v)` (S-009-R47).
///
/// # Safety
/// C-ABI — `d`/`m`/`v` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn matrix4_translate3v(d: *mut Matrix4, m: *const Matrix4, v: *const Vec3) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let m = *m;
        let mut val = *d;
        obs_math::matrix4::matrix4_translate3v(&mut val, &m, &*v);
        *d = val;
    }
}

/// C `matrix4_translate4v(d, m, v)` (S-009-R47).
///
/// # Safety
/// C-ABI — `d`/`m`/`v` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn matrix4_translate4v(d: *mut Matrix4, m: *const Matrix4, v: *const Vec4) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let m = *m;
        let mut val = *d;
        obs_math::matrix4::matrix4_translate4v(&mut val, &m, &*v);
        *d = val;
    }
}

/// C `matrix4_rotate(d, m, q)` (S-009-R48).
///
/// # Safety
/// C-ABI — `d`/`m`/`q` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn matrix4_rotate(d: *mut Matrix4, m: *const Matrix4, q: *const Quat) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let m = *m;
        let mut val = *d;
        obs_math::matrix4::matrix4_rotate(&mut val, &m, &*q);
        *d = val;
    }
}

/// C `matrix4_rotate_aa(d, m, aa)` (S-009-R48).
///
/// # Safety
/// C-ABI — `d`/`m`/`aa` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn matrix4_rotate_aa(d: *mut Matrix4, m: *const Matrix4, aa: *const AxisAng) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let m = *m;
        let mut val = *d;
        obs_math::matrix4::matrix4_rotate_aa(&mut val, &m, &*aa);
        *d = val;
    }
}

/// C `matrix4_scale(d, m, v)` (S-009-R49).
///
/// # Safety
/// C-ABI — `d`/`m`/`v` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn matrix4_scale(d: *mut Matrix4, m: *const Matrix4, v: *const Vec3) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let m = *m;
        let mut val = *d;
        obs_math::matrix4::matrix4_scale(&mut val, &m, &*v);
        *d = val;
    }
}

/// C `matrix4_translate3v_i(d, v, m)` (S-009-R50).
///
/// # Safety
/// C-ABI — `d`/`v`/`m` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn matrix4_translate3v_i(d: *mut Matrix4, v: *const Vec3, m: *const Matrix4) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let mut val = *d;
        obs_math::matrix4::matrix4_translate3v_i(&mut val, &*v, &*m);
        *d = val;
    }
}

/// C `matrix4_translate4v_i(d, v, m)` (S-009-R50).
///
/// # Safety
/// C-ABI — `d`/`v`/`m` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn matrix4_translate4v_i(d: *mut Matrix4, v: *const Vec4, m: *const Matrix4) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let mut val = *d;
        obs_math::matrix4::matrix4_translate4v_i(&mut val, &*v, &*m);
        *d = val;
    }
}

/// C `matrix4_rotate_i(d, q, m)` (S-009-R50).
///
/// # Safety
/// C-ABI — `d`/`q`/`m` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn matrix4_rotate_i(d: *mut Matrix4, q: *const Quat, m: *const Matrix4) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let mut val = *d;
        obs_math::matrix4::matrix4_rotate_i(&mut val, &*q, &*m);
        *d = val;
    }
}

/// C `matrix4_rotate_aa_i(d, aa, m)` (S-009-R50).
///
/// # Safety
/// C-ABI — `d`/`aa`/`m` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn matrix4_rotate_aa_i(
    d: *mut Matrix4,
    aa: *const AxisAng,
    m: *const Matrix4,
) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let mut val = *d;
        obs_math::matrix4::matrix4_rotate_aa_i(&mut val, &*aa, &*m);
        *d = val;
    }
}

/// C `matrix4_scale_i(d, v, m)` (S-009-R50).
///
/// # Safety
/// C-ABI — `d`/`v`/`m` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn matrix4_scale_i(d: *mut Matrix4, v: *const Vec3, m: *const Matrix4) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let mut val = *d;
        obs_math::matrix4::matrix4_scale_i(&mut val, &*v, &*m);
        *d = val;
    }
}

/// C `matrix4_inv(d, m)` (S-009-R51) — returns whether `m` was
/// invertible (C `bool`).
///
/// # Safety
/// C-ABI — `d`/`m` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn matrix4_inv(d: *mut Matrix4, m: *const Matrix4) -> bool {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let mut val = *d;
        let r = obs_math::matrix4::matrix4_inv(&mut val, &*m);
        *d = val;
        r
    }
}

/// C `matrix4_transpose(d, m)` (S-009-R52).
///
/// # Safety
/// C-ABI — `d`/`m` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn matrix4_transpose(d: *mut Matrix4, m: *const Matrix4) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let mut val = *d;
        obs_math::matrix4::matrix4_transpose(&mut val, &*m);
        *d = val;
    }
}

// ── plane.c (7) ─────────────────────────────────────────────────────────

/// C `plane_from_tri(d, v1, v2, v3)` (S-010-R53).
///
/// # Safety
/// C-ABI — all pointers valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn plane_from_tri(
    d: *mut Plane,
    v1: *const Vec3,
    v2: *const Vec3,
    v3: *const Vec3,
) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let v1 = *v1;
        let v2 = *v2;
        let v3 = *v3;
        let mut val = *d;
        obs_math::plane::plane_from_tri(&mut val, &v1, &v2, &v3);
        *d = val;
    }
}

/// C `plane_transform(d, p, m)` (S-010-R54).
///
/// # Safety
/// C-ABI — `d`/`p`/`m` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn plane_transform(d: *mut Plane, p: *const Plane, m: *const Matrix4) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let p = *p;
        let mut val = *d;
        obs_math::plane::plane_transform(&mut val, &p, &*m);
        *d = val;
    }
}

/// C `plane_transform3x4(d, p, m)` (S-010-R54).
///
/// # Safety
/// C-ABI — `d`/`p`/`m` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn plane_transform3x4(d: *mut Plane, p: *const Plane, m: *const Matrix3) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let p = *p;
        let mut val = *d;
        obs_math::plane::plane_transform3x4(&mut val, &p, &*m);
        *d = val;
    }
}

/// C `plane_intersection_ray(p, orig, dir, t)` (S-010-R55).
///
/// # Safety
/// C-ABI — `p`/`orig`/`dir`/`t` are valid for the call (the C callers
/// always pass a live `t` slot).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn plane_intersection_ray(
    p: *const Plane,
    orig: *const Vec3,
    dir: *const Vec3,
    t: *mut f32,
) -> bool {
    // SAFETY: read-compute-write; the `t` slot is written only through
    unsafe {
        // the caller's own pointer (module note).
        let mut t_val = *t;
        let r = obs_math::plane::plane_intersection_ray(&*p, &*orig, &*dir, &mut t_val);
        *t = t_val;
        r
    }
}

/// C `plane_intersection_line(p, v1, v2, t)` (S-010-R56).
///
/// # Safety
/// C-ABI — all pointers valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn plane_intersection_line(
    p: *const Plane,
    v1: *const Vec3,
    v2: *const Vec3,
    t: *mut f32,
) -> bool {
    // SAFETY: read-compute-write (module note).
    unsafe {
        let mut t_val = *t;
        let r = obs_math::plane::plane_intersection_line(&*p, &*v1, &*v2, &mut t_val);
        *t = t_val;
        r
    }
}

/// C `plane_tri_inside(p, v1, v2, v3, precision)` (S-010-R57).
///
/// # Safety
/// C-ABI — all pointers valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn plane_tri_inside(
    p: *const Plane,
    v1: *const Vec3,
    v2: *const Vec3,
    v3: *const Vec3,
    precision: f32,
) -> bool {
    // SAFETY: read-only (module note).
    unsafe { obs_math::plane::plane_tri_inside(&*p, &*v1, &*v2, &*v3, precision) }
}

/// C `plane_line_inside(p, v1, v2, precision)` (S-010-R58).
///
/// # Safety
/// C-ABI — all pointers valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn plane_line_inside(
    p: *const Plane,
    v1: *const Vec3,
    v2: *const Vec3,
    precision: f32,
) -> bool {
    // SAFETY: read-only (module note).
    unsafe { obs_math::plane::plane_line_inside(&*p, &*v1, &*v2, precision) }
}

// ── bounds.c (16) ───────────────────────────────────────────────────────

/// C `bounds_move(d, b, v)` (S-010-R59).
///
/// # Safety
/// C-ABI — `d`/`b`/`v` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bounds_move(d: *mut Bounds, b: *const Bounds, v: *const Vec3) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let b = *b;
        let mut val = *d;
        obs_math::bounds::bounds_move(&mut val, &b, &*v);
        *d = val;
    }
}

/// C `bounds_scale(d, b, v)` (S-010-R60).
///
/// # Safety
/// C-ABI — `d`/`b`/`v` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bounds_scale(d: *mut Bounds, b: *const Bounds, v: *const Vec3) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let b = *b;
        let mut val = *d;
        obs_math::bounds::bounds_scale(&mut val, &b, &*v);
        *d = val;
    }
}

/// C `bounds_merge(d, a, b)` (S-010-R61).
///
/// # Safety
/// C-ABI — `d`/`a`/`b` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bounds_merge(d: *mut Bounds, a: *const Bounds, b: *const Bounds) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let a = *a;
        let b = *b;
        let mut val = *d;
        obs_math::bounds::bounds_merge(&mut val, &a, &b);
        *d = val;
    }
}

/// C `bounds_merge_point(d, b, v)` (S-010-R62).
///
/// # Safety
/// C-ABI — `d`/`b`/`v` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bounds_merge_point(d: *mut Bounds, b: *const Bounds, v: *const Vec3) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let b = *b;
        let mut val = *d;
        obs_math::bounds::bounds_merge_point(&mut val, &b, &*v);
        *d = val;
    }
}

/// C `bounds_get_point(dst, b, i)` (S-010-R63, Q-7).
///
/// # Safety
/// C-ABI — `dst`/`b` are valid for the call (`dst` may point inside `b`;
/// the copy-out pattern keeps the writes sequential).
#[allow(clippy::unnecessary_cast)] // c_uint is u32 here; the cast keeps the portability intent
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bounds_get_point(dst: *mut Vec3, b: *const Bounds, i: c_uint) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let b = *b;
        let mut val = *dst;
        obs_math::bounds::bounds_get_point(&mut val, &b, i as u32);
        *dst = val;
    }
}

/// C `bounds_get_center(dst, b)` (S-010-R64).
///
/// # Safety
/// C-ABI — `dst`/`b` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bounds_get_center(dst: *mut Vec3, b: *const Bounds) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let mut val = *dst;
        obs_math::bounds::bounds_get_center(&mut val, &*b);
        *dst = val;
    }
}

/// C `bounds_transform(d, b, m)` (S-010-R65).
///
/// # Safety
/// C-ABI — `d`/`b`/`m` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bounds_transform(d: *mut Bounds, b: *const Bounds, m: *const Matrix4) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let b = *b;
        let mut val = *d;
        obs_math::bounds::bounds_transform(&mut val, &b, &*m);
        *d = val;
    }
}

/// C `bounds_transform3x4(d, b, m)` (S-010-R65).
///
/// # Safety
/// C-ABI — `d`/`b`/`m` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bounds_transform3x4(d: *mut Bounds, b: *const Bounds, m: *const Matrix3) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let b = *b;
        let mut val = *d;
        obs_math::bounds::bounds_transform3x4(&mut val, &b, &*m);
        *d = val;
    }
}

/// C `bounds_intersection_ray(b, orig, dir, t)` (S-010-R66).
///
/// # Safety
/// C-ABI — all pointers valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bounds_intersection_ray(
    b: *const Bounds,
    orig: *const Vec3,
    dir: *const Vec3,
    t: *mut f32,
) -> bool {
    // SAFETY: read-compute-write (module note).
    unsafe {
        let mut t_val = *t;
        let r = obs_math::bounds::bounds_intersection_ray(&*b, &*orig, &*dir, &mut t_val);
        *t = t_val;
        r
    }
}

/// C `bounds_intersection_line(b, p1, p2, t)` (S-010-R67).
///
/// # Safety
/// C-ABI — all pointers valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bounds_intersection_line(
    b: *const Bounds,
    p1: *const Vec3,
    p2: *const Vec3,
    t: *mut f32,
) -> bool {
    // SAFETY: read-compute-write (module note).
    unsafe {
        let mut t_val = *t;
        let r = obs_math::bounds::bounds_intersection_line(&*b, &*p1, &*p2, &mut t_val);
        *t = t_val;
        r
    }
}

/// C `bounds_plane_test(b, p)` (S-010-R68, the D-2 constant is kept).
///
/// # Safety
/// C-ABI — `b`/`p` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bounds_plane_test(b: *const Bounds, p: *const Plane) -> bool {
    // SAFETY: read-only (module note).
    unsafe { obs_math::bounds::bounds_plane_test(&*b, &*p) }
}

/// C `bounds_under_plane(b, p)` (S-010-R69).
///
/// # Safety
/// C-ABI — `b`/`p` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bounds_under_plane(b: *const Bounds, p: *const Plane) -> bool {
    // SAFETY: read-only (module note).
    unsafe { obs_math::bounds::bounds_under_plane(&*b, &*p) }
}

/// C `bounds_intersects(b, test, eps)` (S-010-R70).
///
/// # Safety
/// C-ABI — `b`/`test` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bounds_intersects(
    b: *const Bounds,
    test: *const Bounds,
    eps: f32,
) -> bool {
    // SAFETY: read-only (module note).
    unsafe { obs_math::bounds::bounds_intersects(&*b, &*test, eps) }
}

/// C `bounds_intersects_obb(b, test, m, eps)` (S-010-R71).
///
/// # Safety
/// C-ABI — all pointers valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bounds_intersects_obb(
    b: *const Bounds,
    test: *const Bounds,
    m: *const Matrix4,
    eps: f32,
) -> bool {
    // SAFETY: read-only (module note).
    unsafe { obs_math::bounds::bounds_intersects_obb(&*b, &*test, &*m, eps) }
}

/// C `bounds_intersects_obb3x4(b, test, m, eps)` (S-010-R71).
///
/// # Safety
/// C-ABI — all pointers valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bounds_intersects_obb3x4(
    b: *const Bounds,
    test: *const Bounds,
    m: *const Matrix3,
    eps: f32,
) -> bool {
    // SAFETY: read-only (module note).
    unsafe { obs_math::bounds::bounds_intersects_obb3x4(&*b, &*test, &*m, eps) }
}

/// C `bounds_min_dist(b, p)` (S-010-R72).
///
/// # Safety
/// C-ABI — `b`/`p` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bounds_min_dist(b: *const Bounds, p: *const Plane) -> f32 {
    // SAFETY: read-only (module note).
    unsafe { obs_math::bounds::bounds_min_dist(&*b, &*p) }
}

// ── math-extra.c (7) ────────────────────────────────────────────────────

/// C `polar_to_cart(d, v)` (S-010-R73).
///
/// # Safety
/// C-ABI — `d`/`v` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn polar_to_cart(d: *mut Vec3, v: *const Vec3) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let mut val = *d;
        obs_math::math_extra::polar_to_cart(&mut val, &*v);
        *d = val;
    }
}

/// C `cart_to_polar(d, v)` (S-010-R74).
///
/// # Safety
/// C-ABI — `d`/`v` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn cart_to_polar(d: *mut Vec3, v: *const Vec3) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let mut val = *d;
        obs_math::math_extra::cart_to_polar(&mut val, &*v);
        *d = val;
    }
}

/// C `norm_to_polar(d, norm)` (S-010-R75).
///
/// # Safety
/// C-ABI — `d`/`norm` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn norm_to_polar(d: *mut Vec2, norm: *const Vec3) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let mut val = *d;
        obs_math::math_extra::norm_to_polar(&mut val, &*norm);
        *d = val;
    }
}

/// C `polar_to_norm(d, polar)` (S-010-R76).
///
/// # Safety
/// C-ABI — `d`/`polar` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn polar_to_norm(d: *mut Vec3, polar: *const Vec2) {
    // SAFETY: copy-in, compute, write-back (module note).
    unsafe {
        let mut val = *d;
        obs_math::math_extra::polar_to_norm(&mut val, &*polar);
        *d = val;
    }
}

/// C `calc_torquef(val1, val2, torque, min_adjust, t)` (S-010-R77).
#[unsafe(no_mangle)]
pub extern "C" fn calc_torquef(val1: f32, val2: f32, torque: f32, min_adjust: f32, t: f32) -> f32 {
    obs_math::math_extra::calc_torquef(val1, val2, torque, min_adjust, t)
}

/// C `calc_torque(d, v1, v2, torque, min_adjust, t)` (S-010-R77).
///
/// Q-9 (S-009): in the C reference the incremental stores corrupt the
/// result when `d` aliases `v1` — after `vec3_mulf(d, dir, adj)` has
/// clobbered `v1`, `vec3_add(d, d, v1)` reads the clobbered value, so
/// the result is `2 * dir * adj`. The Rust core reads `v1` before the
/// store (documented deviation — unreachable from a Rust caller); the C
/// ABI reproduces the C semantics when the pointers alias.
///
/// # Safety
/// C-ABI — `d`/`v1`/`v2` are valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn calc_torque(
    d: *mut Vec3,
    v1: *const Vec3,
    v2: *const Vec3,
    torque: f32,
    min_adjust: f32,
    t: f32,
) {
    // SAFETY: copy-in, compute, write-back (module note); the aliasing
    // case recomputes the C corruption before the write.
    unsafe {
        let v1_ptr = v1;
        let v1 = *v1;
        let v2 = *v2;
        let mut val = *d;
        obs_math::math_extra::calc_torque(&mut val, &v1, &v2, torque, min_adjust, t);
        if std::ptr::eq(d, v1_ptr) && !obs_math::vec3_close(&v1, &v2, obs_math::EPSILON) {
            let line = obs_math::vec3::vec3_sub(&v2, &v1);
            let od = obs_math::vec3::vec3_len(&line);
            let dir = obs_math::vec3::vec3_mulf(&line, 1.0 / od);
            let mut td = od * torque;
            if td < min_adjust {
                td = min_adjust;
            }
            let adj = td * t;
            if adj <= od - obs_math::LARGE_EPSILON {
                let step = obs_math::vec3::vec3_mulf(&dir, adj);
                val = obs_math::vec3::vec3_add(&step, &step);
            } else {
                val = v2; // the overshoot copy is aliasing-safe in C too
            }
        }
        *d = val;
    }
}

/// C `rand_float(positive_only)` (S-010-R78).
#[unsafe(no_mangle)]
pub extern "C" fn rand_float(positive_only: c_int) -> f32 {
    obs_math::math_extra::rand_float(positive_only != 0)
}
