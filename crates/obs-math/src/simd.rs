//! SIMD intrinsic primitives (P1-19; C reference: `libobs/util/sse-intrin.h`).
//!
//! The graphics math headers (`vec3.h`, `vec4.h`, `quat.h`) and the two
//! `matrix3.c`/`matrix4.c` transpose paths run on 128-bit packed-float
//! vectors (`__m128`, the `m` union member) through SSE2 intrinsics on x86
//! and simde's emulation elsewhere (`sse-intrin.h` selects the backend).
//! This module ports exactly the set of intrinsics the C code uses, as a
//! self-contained `std::arch` implementation with a per-op scalar fallback.
//! It is consumed by the future Rust graphics plane (Phase 3, M5); the
//! scalar `Vec3`/`Vec4`/`Quat`/`Matrix*` ports in this crate keep the C-ABI
//! layout and stay the reference semantics.
//!
//! Placement decision (P1-19.2): a module in `obs-math`, not a new crate —
//! the consumers are the math/graphics types that already live here, the
//! module has no dependencies of its own, and a separate crate would add
//! workspace wiring for no surface. It is not glob-re-exported at the crate
//! root on purpose: the low-level `*_ps` names stay namespaced as
//! `obs_math::simd::*`.
//!
//! # P1-19.1 inventory (C intrinsics in use)
//!
//! 14 SSE2 float intrinsics, 96 call sites (verified against the pinned
//! 33.0.1 tree):
//!
//! | C intrinsic        | sites | C users (all in `libobs/graphics/`)                                             | Rust (this module) |
//! | ------------------ | ----- | ------------------------------------------------------------------------------ | ------------------ |
//! | `_mm_add_ps`       | 12    | `vec3_add/addf`, `vec4_add/addf`, `quat_add/addf`, the dot idiom in `*_dot` (×3) | `add_ps`           |
//! | `_mm_sub_ps`       | 7     | `vec3_sub/subf`, `vec4_sub/subf`, `quat_sub/subf`                                | `sub_ps`           |
//! | `_mm_mul_ps`       | 13    | `vec3_mul/mulf/norm/cross`, `vec4_mul/mulf/norm`, `quat_mulf/norm`, dot idiom    | `mul_ps`           |
//! | `_mm_div_ps`       | 5     | `vec3_div/divf`, `vec4_div/divf`, `quat_divf`                                    | `div_ps`           |
//! | `_mm_min_ps`       | 4     | `vec3_min/minf`, `vec4_min/minf`                                                 | `min_ps`           |
//! | `_mm_max_ps`       | 4     | `vec3_max/maxf`, `vec4_max/maxf`                                                 | `max_ps`           |
//! | `_mm_set1_ps`      | 19    | the ±f ops, `minf/maxf` and `norm` of `vec3`/`vec4`/`quat`                       | `set1_ps`          |
//! | `_mm_set_ps`       | 3     | `vec3_set`, `vec4_set`, `quat_set`                                               | `set_ps`           |
//! | `_mm_setzero_ps`   | 6     | `*_zero`, `quat_identity`, the zero branch of `*_norm`                           | `setzero_ps`       |
//! | `_mm_shuffle_ps`   | 10    | dot idiom (mask `0x55` ×3), `vec3_cross` (`0xC9`, `0xD2`), `matrix3_transpose` (`0xC8`, `0xDD`, `0xE8`) | `shuffle_ps` |
//! | `_mm_movehl_ps`    | 4     | dot idiom (×3), `matrix3_transpose`                                              | `movehl_ps`        |
//! | `_mm_movelh_ps`    | 1     | `matrix3_transpose`                                                              | `movelh_ps`        |
//! | `_mm_unpacklo_ps`  | 4     | `matrix4_transpose`                                                              | `unpacklo_ps`      |
//! | `_mm_unpackhi_ps`  | 4     | `matrix4_transpose`                                                              | `unpackhi_ps`      |
//!
//! The compound C functions built from them (`vec3_dot`/`vec4_dot`/
//! `quat_dot`, `vec3_cross`, `matrix3_transpose`, `matrix4_transpose`)
//! compose these primitives; `dot_ps` ports the dot idiom verbatim.
//! Intrinsics used by other components (`obs-audio-controls.c`,
//! `format-conversion.c`, the capture plugins) are owned by their own
//! subtasks, not by P1-19.
//!
//! # Dispatch (P1-19.2)
//!
//! - x86_64: SSE2 is the mandatory baseline; the runtime
//!   `is_x86_feature_detected!` check (once, cached) keeps the scalar
//!   fallback reachable per the spec. The batch path (`binop_slice`) uses
//!   AVX2 behind the same runtime check, with an SSE2 tail and a scalar
//!   tail.
//! - aarch64: NEON, which is mandatory in the AArch64 ISA (no runtime
//!   check; `std::arch` keeps the calls `unsafe` behind the target-feature
//!   gate).
//! - any other target, and Miri (the caps report as absent there): scalar.
//!
//! # Bit-exactness
//!
//! Every op is bit-identical to the C intrinsic on the same architecture
//! (the randomized tests compare each op against its `*_scalar` twin over
//! random full `f32` bit patterns plus a curated edge set). The `min`/
//! `max` NaN edge is uniform across architectures: the ordered comparison
//! is false when *either* operand is NaN, so both the x86 `minps`/`maxps`
//! hardware (verified) and simde's aarch64 emulation (`a < b ? a : b`)
//! return the second operand — even when that second operand is itself the
//! NaN. (The Intel intrinsics guide's "the non-NaN operand is returned"
//! wording does not hold for a second-operand NaN; the first draft encoded
//! that wording on x86_64 and the CI round 1 caught it.)

#[cfg(target_arch = "x86_64")]
use std::sync::OnceLock;

/// 128-bit packed single-precision vector — the Rust counterpart of the C
/// `__m128` (`vec3.h`/`vec4.h`/`quat.h` `m` member). `#[repr(C, align(16))]`
/// makes it layout-identical to `__m128`, `float32x4_t` and this crate's
/// 16-byte `Vec3`/`Vec4`/`Quat` structs.
#[repr(C, align(16))]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct M128F {
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub w: f32,
}

impl M128F {
    /// The four lanes in memory order (`[x, y, z, w]`).
    #[must_use]
    pub const fn from_lanes(lanes: [f32; 4]) -> Self {
        Self {
            x: lanes[0],
            y: lanes[1],
            z: lanes[2],
            w: lanes[3],
        }
    }

    /// The four lanes in memory order.
    #[must_use]
    pub const fn as_lanes(self) -> [f32; 4] {
        [self.x, self.y, self.z, self.w]
    }

    /// All four lanes the same value.
    #[must_use]
    pub const fn splat(v: f32) -> Self {
        Self {
            x: v,
            y: v,
            z: v,
            w: v,
        }
    }

    /// All-zero lanes.
    #[must_use]
    pub const fn zero() -> Self {
        Self::splat(0.0)
    }

    #[cfg(target_arch = "x86_64")]
    fn to_x86(self) -> std::arch::x86_64::__m128 {
        // SAFETY: `M128F` is `#[repr(C, align(16))]` with four `f32` lanes and
        // no padding — layout-identical to `__m128` (128 bits).
        unsafe { core::mem::transmute(self) }
    }

    #[cfg(target_arch = "x86_64")]
    fn from_x86(v: std::arch::x86_64::__m128) -> Self {
        // SAFETY: same 16-byte layout as `to_x86`.
        unsafe { core::mem::transmute(v) }
    }

    #[cfg(target_arch = "aarch64")]
    fn to_neon(self) -> std::arch::aarch64::float32x4_t {
        // SAFETY: `M128F` is layout-identical to `float32x4_t` (four f32
        // lanes, 16 bytes).
        unsafe { core::mem::transmute(self) }
    }

    #[cfg(target_arch = "aarch64")]
    fn from_neon(v: std::arch::aarch64::float32x4_t) -> Self {
        // SAFETY: same 16-byte layout as `to_neon`.
        unsafe { core::mem::transmute(v) }
    }
}

/// P1-19.2: the runtime feature check, cached. SSE2 is the x86_64 baseline
/// so `sse2` is always true on real hardware; the check is kept per the spec
/// so the scalar fallback stays reachable. Under Miri all caps are absent so
/// the scalar path runs (Miri does not implement the SIMD instruction sets).
#[cfg(target_arch = "x86_64")]
#[derive(Clone, Copy, Debug)]
struct CpuCaps {
    sse2: bool,
    avx2: bool,
}

#[cfg(target_arch = "x86_64")]
fn caps() -> CpuCaps {
    static CAPS: OnceLock<CpuCaps> = OnceLock::new();
    *CAPS.get_or_init(|| {
        if cfg!(miri) {
            CpuCaps {
                sse2: false,
                avx2: false,
            }
        } else {
            CpuCaps {
                sse2: std::arch::is_x86_feature_detected!("sse2"),
                avx2: std::arch::is_x86_feature_detected!("avx2"),
            }
        }
    })
}

/// `_mm_setzero_ps` — all lanes `0.0`. Direct construction (the C intrinsic
/// only loads a constant, so there is no SIMD path to dispatch to).
#[must_use]
pub const fn setzero_ps() -> M128F {
    M128F::zero()
}

/// `_mm_set1_ps` — all lanes `f`. Direct construction, as in `setzero_ps`.
#[must_use]
pub const fn set1_ps(f: f32) -> M128F {
    M128F::splat(f)
}

/// `_mm_set_ps(w, z, y, x)` — C argument order (`e3` first): `x` lands in
/// lane 0, `w` in lane 3. Direct construction, as in `setzero_ps`.
#[must_use]
pub const fn set_ps(w: f32, z: f32, y: f32, x: f32) -> M128F {
    M128F { x, y, z, w }
}

/// `_mm_add_ps` — per-lane add.
#[must_use]
pub fn add_ps(a: M128F, b: M128F) -> M128F {
    #[cfg(target_arch = "x86_64")]
    {
        if caps().sse2 {
            // SAFETY: SSE2 is the x86_64 baseline ISA (checked at
            // runtime via `caps()` where relevant); the stdarch call is
            // unsafe only behind its target-feature gate.
            let r = unsafe { std::arch::x86_64::_mm_add_ps(a.to_x86(), b.to_x86()) };
            return M128F::from_x86(r);
        }
        add_ps_scalar(a, b)
    }
    #[cfg(target_arch = "aarch64")]
    {
        // SAFETY: NEON is mandatory in the AArch64 ISA.
        let r = unsafe { std::arch::aarch64::vaddq_f32(a.to_neon(), b.to_neon()) };
        M128F::from_neon(r)
    }
    #[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64")))]
    add_ps_scalar(a, b)
}

/// Scalar twin of [`add_ps`] (also the scalar baseline of the P1-19.3
/// benchmarks).
#[must_use]
pub fn add_ps_scalar(a: M128F, b: M128F) -> M128F {
    M128F::from_lanes([a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w])
}

/// `_mm_sub_ps` — per-lane subtract.
#[must_use]
pub fn sub_ps(a: M128F, b: M128F) -> M128F {
    #[cfg(target_arch = "x86_64")]
    {
        if caps().sse2 {
            // SAFETY: SSE2 is the x86_64 baseline ISA (checked at
            // runtime via `caps()` where relevant); the stdarch call is
            // unsafe only behind its target-feature gate.
            let r = unsafe { std::arch::x86_64::_mm_sub_ps(a.to_x86(), b.to_x86()) };
            return M128F::from_x86(r);
        }
        sub_ps_scalar(a, b)
    }
    #[cfg(target_arch = "aarch64")]
    {
        // SAFETY: NEON is mandatory in the AArch64 ISA.
        let r = unsafe { std::arch::aarch64::vsubq_f32(a.to_neon(), b.to_neon()) };
        M128F::from_neon(r)
    }
    #[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64")))]
    sub_ps_scalar(a, b)
}

/// Scalar twin of [`sub_ps`].
#[must_use]
pub fn sub_ps_scalar(a: M128F, b: M128F) -> M128F {
    M128F::from_lanes([a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w])
}

/// `_mm_mul_ps` — per-lane multiply.
#[must_use]
pub fn mul_ps(a: M128F, b: M128F) -> M128F {
    #[cfg(target_arch = "x86_64")]
    {
        if caps().sse2 {
            // SAFETY: SSE2 is the x86_64 baseline ISA (checked at
            // runtime via `caps()` where relevant); the stdarch call is
            // unsafe only behind its target-feature gate.
            let r = unsafe { std::arch::x86_64::_mm_mul_ps(a.to_x86(), b.to_x86()) };
            return M128F::from_x86(r);
        }
        mul_ps_scalar(a, b)
    }
    #[cfg(target_arch = "aarch64")]
    {
        // SAFETY: NEON is mandatory in the AArch64 ISA.
        let r = unsafe { std::arch::aarch64::vmulq_f32(a.to_neon(), b.to_neon()) };
        M128F::from_neon(r)
    }
    #[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64")))]
    mul_ps_scalar(a, b)
}

/// Scalar twin of [`mul_ps`].
#[must_use]
pub fn mul_ps_scalar(a: M128F, b: M128F) -> M128F {
    M128F::from_lanes([a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w])
}

/// `_mm_div_ps` — per-lane IEEE divide.
#[must_use]
pub fn div_ps(a: M128F, b: M128F) -> M128F {
    #[cfg(target_arch = "x86_64")]
    {
        if caps().sse2 {
            // SAFETY: SSE2 is the x86_64 baseline ISA (checked at
            // runtime via `caps()` where relevant); the stdarch call is
            // unsafe only behind its target-feature gate.
            let r = unsafe { std::arch::x86_64::_mm_div_ps(a.to_x86(), b.to_x86()) };
            return M128F::from_x86(r);
        }
        div_ps_scalar(a, b)
    }
    #[cfg(target_arch = "aarch64")]
    {
        // SAFETY: NEON is mandatory in the AArch64 ISA.
        let r = unsafe { std::arch::aarch64::vdivq_f32(a.to_neon(), b.to_neon()) };
        M128F::from_neon(r)
    }
    #[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64")))]
    div_ps_scalar(a, b)
}

/// Scalar twin of [`div_ps`].
#[must_use]
pub fn div_ps_scalar(a: M128F, b: M128F) -> M128F {
    M128F::from_lanes([a.x / b.x, a.y / b.y, a.z / b.z, a.w / b.w])
}

/// `_mm_min_ps` — per-lane minimum. See [`min_lane`] for the NaN/±0
/// semantics (architecture-dependent in the C reference itself — see the
/// module docs).
#[must_use]
pub fn min_ps(a: M128F, b: M128F) -> M128F {
    #[cfg(target_arch = "x86_64")]
    {
        if caps().sse2 {
            // SAFETY: SSE2 is the x86_64 baseline ISA (checked at
            // runtime via `caps()` where relevant); the stdarch call is
            // unsafe only behind its target-feature gate.
            let r = unsafe { std::arch::x86_64::_mm_min_ps(a.to_x86(), b.to_x86()) };
            return M128F::from_x86(r);
        }
        min_ps_scalar(a, b)
    }
    #[cfg(target_arch = "aarch64")]
    {
        let (a, b) = (a.to_neon(), b.to_neon());
        // SAFETY: NEON is mandatory in the AArch64 ISA. The bitselect on an
        // ordered `<` comparison reproduces simde's non-`FAST_NANS`
        // emulation exactly (plain `vminq_f32` would diverge on the
        // NaN/±0 edges).
        let r = unsafe { std::arch::aarch64::vbslq_f32(std::arch::aarch64::vcltq_f32(a, b), a, b) };
        M128F::from_neon(r)
    }
    #[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64")))]
    min_ps_scalar(a, b)
}

/// Per-lane `min` scalar semantics — the plain ordered ternary, on every
/// architecture: `minps` (x86_64) and simde's aarch64 emulation of
/// `_mm_min_ps` are both exactly `(a < b) ? a : b` lane-wise. NaN handling
/// falls out of the ordered comparison being false: **a NaN in either
/// operand returns `b`** (when `b` itself is NaN the result is that NaN —
/// hardware-verified on x86_64, the Intel intrinsics guide's "the
/// non-NaN operand is returned" wording is wrong for `b`-NaN); equal
/// values — including `-0.0` vs `+0.0` — return `b`, the deterministic
/// `minps` result.
#[must_use]
fn min_lane(x: f32, y: f32) -> f32 {
    if x < y { x } else { y }
}

/// Scalar twin of [`min_ps`] (same NaN/±0 semantics).
#[must_use]
pub fn min_ps_scalar(a: M128F, b: M128F) -> M128F {
    M128F::from_lanes([
        min_lane(a.x, b.x),
        min_lane(a.y, b.y),
        min_lane(a.z, b.z),
        min_lane(a.w, b.w),
    ])
}
/// `_mm_max_ps` — per-lane maximum. See [`max_lane`] for the NaN/±0
/// semantics.
#[must_use]
pub fn max_ps(a: M128F, b: M128F) -> M128F {
    #[cfg(target_arch = "x86_64")]
    {
        if caps().sse2 {
            // SAFETY: SSE2 is the x86_64 baseline ISA (checked at
            // runtime via `caps()` where relevant); the stdarch call is
            // unsafe only behind its target-feature gate.
            let r = unsafe { std::arch::x86_64::_mm_max_ps(a.to_x86(), b.to_x86()) };
            return M128F::from_x86(r);
        }
        max_ps_scalar(a, b)
    }
    #[cfg(target_arch = "aarch64")]
    {
        let (a, b) = (a.to_neon(), b.to_neon());
        // SAFETY: NEON is mandatory in the AArch64 ISA; bitselect on an
        // ordered `>` comparison = simde's emulation (see `min_ps`).
        let r = unsafe { std::arch::aarch64::vbslq_f32(std::arch::aarch64::vcgtq_f32(a, b), a, b) };
        M128F::from_neon(r)
    }
    #[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64")))]
    max_ps_scalar(a, b)
}

/// Per-lane `max` scalar semantics — the ordered mirror of [`min_lane`]
/// (`a > b ? a : b`; `maxps` NaN/±0 behaviour is the same: an ordered
/// comparison that is false — NaN in either operand, or equal values —
/// returns `b`).
#[must_use]
fn max_lane(x: f32, y: f32) -> f32 {
    if x > y { x } else { y }
}

/// Scalar twin of [`max_ps`].
#[must_use]
pub fn max_ps_scalar(a: M128F, b: M128F) -> M128F {
    M128F::from_lanes([
        max_lane(a.x, b.x),
        max_lane(a.y, b.y),
        max_lane(a.z, b.z),
        max_lane(a.w, b.w),
    ])
}
/// `_mm_shuffle_ps(a, b, MASK)` — Intel `shufps`, with the mask as a
/// **const generic** (C passes it as an immediate — a compile-time
/// constant — and stdarch's `_mm_shuffle_ps` is const-generic).
/// Lane semantics: `dest[0] = a[mask & 3]`, `dest[1] = a[(mask >> 2) & 3]`,
/// `dest[2] = b[(mask >> 4) & 3]`, `dest[3] = b[(mask >> 6) & 3]`.
/// The C code uses `0x55` (the dot idiom), `0xC9`/`0xD2` (`vec3_cross`)
/// and `0xC8`/`0xDD`/`0xE8` (`matrix3_transpose`); identity is `0x44`.
///
/// On aarch64 and other targets no 4×f32 permute instruction exists
/// (simde emulates the intrinsic lane-by-lane), so the lane-selection
/// port is used.
#[must_use]
pub fn shuffle_ps<const MASK: i32>(a: M128F, b: M128F) -> M128F {
    // A valid `_MM_SHUFFLE(z, y, x, w)` mask is an 8-bit value (each
    // 2-bit field structurally in `0..=3`); above 0xFF is a caller bug
    // (C behavior above is undefined, so is this).
    debug_assert!((0..=0xFF).contains(&MASK), "shuffle mask must be 8-bit");
    #[cfg(target_arch = "x86_64")]
    {
        // `shufps` is SSE1 — baseline on x86_64, so the intrinsic path
        // is unconditional.
        // SAFETY: SSE1 is the x86_64 baseline ISA; the stdarch call is
        // unsafe only behind its target-feature gate.
        let r = unsafe { std::arch::x86_64::_mm_shuffle_ps::<MASK>(a.to_x86(), b.to_x86()) };
        M128F::from_x86(r)
    }
    #[cfg(not(target_arch = "x86_64"))]
    shuffle_ps_scalar(a, b, MASK as u32)
}

/// Scalar lane selection — the exact port of the `shufps` lane mapping
/// (see [`shuffle_ps`]); used on aarch64 and other targets, and the
/// reference in the property tests.
#[must_use]
pub const fn shuffle_ps_scalar(a: M128F, b: M128F, mask: u32) -> M128F {
    let (a, b) = (a.as_lanes(), b.as_lanes());
    M128F::from_lanes([
        a[mask as usize & 3],
        a[(mask >> 2) as usize & 3],
        b[(mask >> 4) as usize & 3],
        b[(mask >> 6) as usize & 3],
    ])
}
/// `_mm_movelh_ps(a, b)` — `[a.x, a.y, b.x, b.y]`.
#[must_use]
pub fn movelh_ps(a: M128F, b: M128F) -> M128F {
    #[cfg(target_arch = "x86_64")]
    {
        if caps().sse2 {
            // SAFETY: SSE2 is the x86_64 baseline ISA (checked at
            // runtime via `caps()` where relevant); the stdarch call is
            // unsafe only behind its target-feature gate.
            let r = unsafe { std::arch::x86_64::_mm_movelh_ps(a.to_x86(), b.to_x86()) };
            return M128F::from_x86(r);
        }
        movelh_ps_scalar(a, b)
    }
    #[cfg(target_arch = "aarch64")]
    {
        // SAFETY: NEON is mandatory in the AArch64 ISA.
        let r = unsafe {
            std::arch::aarch64::vcombine_f32(
                std::arch::aarch64::vget_low_f32(a.to_neon()),
                std::arch::aarch64::vget_low_f32(b.to_neon()),
            )
        };
        M128F::from_neon(r)
    }
    #[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64")))]
    movelh_ps_scalar(a, b)
}

/// Scalar twin of [`movelh_ps`].
#[must_use]
pub fn movelh_ps_scalar(a: M128F, b: M128F) -> M128F {
    M128F::from_lanes([a.x, a.y, b.x, b.y])
}

/// `_mm_movehl_ps(a, b)` — `[b.z, b.w, a.z, a.w]`.
#[must_use]
pub fn movehl_ps(a: M128F, b: M128F) -> M128F {
    #[cfg(target_arch = "x86_64")]
    {
        if caps().sse2 {
            // SAFETY: SSE2 is the x86_64 baseline ISA (checked at
            // runtime via `caps()` where relevant); the stdarch call is
            // unsafe only behind its target-feature gate.
            let r = unsafe { std::arch::x86_64::_mm_movehl_ps(a.to_x86(), b.to_x86()) };
            return M128F::from_x86(r);
        }
        movehl_ps_scalar(a, b)
    }
    #[cfg(target_arch = "aarch64")]
    {
        let (a, b) = (a.to_neon(), b.to_neon());
        // SAFETY: NEON is mandatory in the AArch64 ISA.
        let r = unsafe {
            std::arch::aarch64::vcombine_f32(
                std::arch::aarch64::vget_high_f32(b),
                std::arch::aarch64::vget_high_f32(a),
            )
        };
        M128F::from_neon(r)
    }
    #[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64")))]
    movehl_ps_scalar(a, b)
}

/// Scalar twin of [`movehl_ps`].
#[must_use]
pub fn movehl_ps_scalar(a: M128F, b: M128F) -> M128F {
    M128F::from_lanes([b.z, b.w, a.z, a.w])
}
/// `_mm_unpacklo_ps(a, b)` — `[a.x, b.x, a.y, b.y]`. The NEON equivalent is
/// `vzip1q_f32` — **not** `vtrn1q_f32`, which transposes 64-bit halves
/// (`[a.x, b.x, a.z, b.z]`); simde's C reference uses `vzip1q_f32`.
#[must_use]
pub fn unpacklo_ps(a: M128F, b: M128F) -> M128F {
    #[cfg(target_arch = "x86_64")]
    {
        if caps().sse2 {
            // SAFETY: SSE2 is the x86_64 baseline ISA (checked at
            // runtime via `caps()` where relevant); the stdarch call is
            // unsafe only behind its target-feature gate.
            let r = unsafe { std::arch::x86_64::_mm_unpacklo_ps(a.to_x86(), b.to_x86()) };
            return M128F::from_x86(r);
        }
        unpacklo_ps_scalar(a, b)
    }
    #[cfg(target_arch = "aarch64")]
    {
        // SAFETY: NEON is mandatory in the AArch64 ISA.
        let r = unsafe { std::arch::aarch64::vzip1q_f32(a.to_neon(), b.to_neon()) };
        M128F::from_neon(r)
    }
    #[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64")))]
    unpacklo_ps_scalar(a, b)
}

/// Scalar twin of [`unpacklo_ps`].
#[must_use]
pub fn unpacklo_ps_scalar(a: M128F, b: M128F) -> M128F {
    M128F::from_lanes([a.x, b.x, a.y, b.y])
}

/// `_mm_unpackhi_ps(a, b)` — `[a.z, b.z, a.w, b.w]`. The NEON equivalent is
/// `vzip2q_f32` (see [`unpacklo_ps`]).
#[must_use]
pub fn unpackhi_ps(a: M128F, b: M128F) -> M128F {
    #[cfg(target_arch = "x86_64")]
    {
        if caps().sse2 {
            // SAFETY: SSE2 is the x86_64 baseline ISA (checked at
            // runtime via `caps()` where relevant); the stdarch call is
            // unsafe only behind its target-feature gate.
            let r = unsafe { std::arch::x86_64::_mm_unpackhi_ps(a.to_x86(), b.to_x86()) };
            return M128F::from_x86(r);
        }
        unpackhi_ps_scalar(a, b)
    }
    #[cfg(target_arch = "aarch64")]
    {
        // SAFETY: NEON is mandatory in the AArch64 ISA.
        let r = unsafe { std::arch::aarch64::vzip2q_f32(a.to_neon(), b.to_neon()) };
        M128F::from_neon(r)
    }
    #[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64")))]
    unpackhi_ps_scalar(a, b)
}

/// Scalar twin of [`unpackhi_ps`].
#[must_use]
pub fn unpackhi_ps_scalar(a: M128F, b: M128F) -> M128F {
    M128F::from_lanes([a.z, b.z, a.w, b.w])
}

/// The dot idiom of `vec3_dot`/`vec4_dot`/`quat_dot` verbatim:
/// `mul`; `t = add(movehl(mul, mul), mul)`; `add(shuffle(t, t, 0x55), t)`;
/// lane 0. The sum order is `(p0 + p2) + (p1 + p3)` — bit-identical to the
/// C result, including how NaNs propagate.
#[must_use]
pub fn dot_ps(a: M128F, b: M128F) -> f32 {
    let mul = mul_ps(a, b);
    let t = add_ps(movehl_ps(mul, mul), mul);
    let r = add_ps(shuffle_ps::<0x55>(t, t), t);
    r.x
}

/// Element-wise ops of the batch path.
#[derive(Clone, Copy, Debug)]
pub enum BinOp {
    Add,
    Sub,
    Mul,
    Div,
    Min,
    Max,
}

impl BinOp {
    /// The scalar (per-lane) form — one place for the NaN/±0 semantics of
    /// [`min_lane`]/[`max_lane`].
    #[must_use]
    pub fn scalar(self, x: f32, y: f32) -> f32 {
        match self {
            BinOp::Add => x + y,
            BinOp::Sub => x - y,
            BinOp::Mul => x * y,
            BinOp::Div => x / y,
            BinOp::Min => min_lane(x, y),
            BinOp::Max => max_lane(x, y),
        }
    }
}

/// Element-wise batch op over flat `f32` slices — the P1-19.2 AVX2/NEON
/// bulk path (the C code only uses 128-bit ops, so this is the
/// `std::arch` extension the spec asks for: AVX2 behind the runtime check,
/// NEON on aarch64, scalar everywhere else, each tail falling through to
/// the next level).
///
/// # Panics
///
/// Panics when `a.len() != b.len()` or `out.len() != a.len()`.
pub fn binop_slice(op: BinOp, a: &[f32], b: &[f32], out: &mut [f32]) {
    assert_eq!(
        a.len(),
        b.len(),
        "binop_slice: a and b must have equal length"
    );
    assert_eq!(
        out.len(),
        a.len(),
        "binop_slice: out must have the input length"
    );

    #[cfg(target_arch = "x86_64")]
    {
        let c = caps();
        let mut i = 0;
        if c.avx2 {
            while i + 8 <= a.len() {
                // SAFETY: AVX2 was runtime-detected in `caps`; `i` is a
                // multiple of 8 and `i + 8 <= len`, so the 32-byte accesses
                // are in bounds for all three slices.
                unsafe {
                    let x = std::arch::x86_64::_mm256_loadu_ps(a.as_ptr().add(i));
                    let y = std::arch::x86_64::_mm256_loadu_ps(b.as_ptr().add(i));
                    let r = match op {
                        BinOp::Add => std::arch::x86_64::_mm256_add_ps(x, y),
                        BinOp::Sub => std::arch::x86_64::_mm256_sub_ps(x, y),
                        BinOp::Mul => std::arch::x86_64::_mm256_mul_ps(x, y),
                        BinOp::Div => std::arch::x86_64::_mm256_div_ps(x, y),
                        BinOp::Min => std::arch::x86_64::_mm256_min_ps(x, y),
                        BinOp::Max => std::arch::x86_64::_mm256_max_ps(x, y),
                    };
                    std::arch::x86_64::_mm256_storeu_ps(out.as_mut_ptr().add(i), r);
                }
                i += 8;
            }
        }
        if c.sse2 {
            while i + 4 <= a.len() {
                // SAFETY: SSE2 baseline (runtime-checked); `i + 4 <= len`
                // keeps the 16-byte accesses in bounds.
                unsafe {
                    let x = std::arch::x86_64::_mm_loadu_ps(a.as_ptr().add(i));
                    let y = std::arch::x86_64::_mm_loadu_ps(b.as_ptr().add(i));
                    let r = match op {
                        BinOp::Add => std::arch::x86_64::_mm_add_ps(x, y),
                        BinOp::Sub => std::arch::x86_64::_mm_sub_ps(x, y),
                        BinOp::Mul => std::arch::x86_64::_mm_mul_ps(x, y),
                        BinOp::Div => std::arch::x86_64::_mm_div_ps(x, y),
                        BinOp::Min => std::arch::x86_64::_mm_min_ps(x, y),
                        BinOp::Max => std::arch::x86_64::_mm_max_ps(x, y),
                    };
                    std::arch::x86_64::_mm_storeu_ps(out.as_mut_ptr().add(i), r);
                }
                i += 4;
            }
        }
        for k in i..a.len() {
            out[k] = op.scalar(a[k], b[k]);
        }
    }

    #[cfg(target_arch = "aarch64")]
    {
        let mut i = 0;
        while i + 4 <= a.len() {
            // SAFETY: NEON is mandatory in the AArch64 ISA; `i + 4 <= len`
            // keeps the 16-byte accesses in bounds (NEON loads/stores are
            // unaligned anyway).
            unsafe {
                let x = std::arch::aarch64::vld1q_f32(a.as_ptr().add(i));
                let y = std::arch::aarch64::vld1q_f32(b.as_ptr().add(i));
                let r = match op {
                    BinOp::Add => std::arch::aarch64::vaddq_f32(x, y),
                    BinOp::Sub => std::arch::aarch64::vsubq_f32(x, y),
                    BinOp::Mul => std::arch::aarch64::vmulq_f32(x, y),
                    BinOp::Div => std::arch::aarch64::vdivq_f32(x, y),
                    // Bitselect on ordered comparisons: simde's C-reference
                    // semantics for min/max (see `min_ps`).
                    BinOp::Min => {
                        std::arch::aarch64::vbslq_f32(std::arch::aarch64::vcltq_f32(x, y), x, y)
                    }
                    BinOp::Max => {
                        std::arch::aarch64::vbslq_f32(std::arch::aarch64::vcgtq_f32(x, y), x, y)
                    }
                };
                std::arch::aarch64::vst1q_f32(out.as_mut_ptr().add(i), r);
            }
            i += 4;
        }
        for k in i..a.len() {
            out[k] = op.scalar(a[k], b[k]);
        }
    }

    #[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64")))]
    for (k, (x, y)) in out.iter_mut().zip(a.iter().zip(b.iter())) {
        *k = op.scalar(*x, *y);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// SplitMix64 — deterministic test PRNG (no dependency).
    struct Rng(u64);

    impl Rng {
        fn next_u64(&mut self) -> u64 {
            let mut z = self.0.wrapping_add(0x9E37_79B9_7F4A_7C15);
            z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
            z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
            z ^ (z >> 31)
        }

        fn f32_bits(&mut self) -> f32 {
            f32::from_bits(self.next_u64() as u32)
        }
    }

    fn random_pair(rng: &mut Rng) -> (M128F, M128F) {
        (
            M128F::from_lanes([
                rng.f32_bits(),
                rng.f32_bits(),
                rng.f32_bits(),
                rng.f32_bits(),
            ]),
            M128F::from_lanes([
                rng.f32_bits(),
                rng.f32_bits(),
                rng.f32_bits(),
                rng.f32_bits(),
            ]),
        )
    }

    fn bits_of(v: M128F) -> [u32; 4] {
        v.as_lanes().map(f32::to_bits)
    }

    /// Bit-exact comparison — for the selection/data-movement ops
    /// (`min`/`max`/`movelh`/`movehl`/`unpacklo`/`unpackhi`/`shuffle`),
    /// whose results always copy an input lane bit-for-bit.
    fn assert_bits_equal(got: M128F, want: M128F) {
        assert_eq!(bits_of(got), bits_of(want), "bit pattern mismatch");
    }

    /// Comparison for the producing arithmetic ops (`add`/`sub`/`mul`/
    /// `div`): exact bits where both sides are non-NaN, NaN-ness where
    /// either is NaN — IEEE leaves the payload/sign of a *produced* NaN
    /// (0/0, `inf - inf`, `inf * 0`) unspecified, and Miri's NEON
    /// emulation of those cases is sign-nondeterministic, while real
    /// hardware (and the C reference) is canonical.
    fn assert_arith_equal(got: M128F, want: M128F) {
        for (i, (g, w)) in bits_of(got).iter().zip(bits_of(want).iter()).enumerate() {
            let (g, w) = (f32::from_bits(*g), f32::from_bits(*w));
            if g.is_nan() || w.is_nan() {
                assert!(
                    g.is_nan() && w.is_nan(),
                    "lane {i}: NaN mismatch {got:?} vs {want:?}"
                );
            } else {
                assert_eq!(g, w, "lane {i}: bit mismatch {got:?} vs {want:?}");
            }
        }
    }

    /// The edge set: ±0, ±1, inf, NaN (quiet, payload, signaling),
    /// denormals, and the finite extremes.
    const EDGES: [f32; 16] = [
        0.0,
        -0.0,
        1.0,
        -1.0,
        2.0,
        0.5,
        f32::INFINITY,
        f32::NEG_INFINITY,
        f32::NAN,
        f32::from_bits(0x7FC0_0001), // quiet NaN with a payload
        f32::from_bits(0x7FA5_5555), // signaling NaN
        f32::MIN_POSITIVE,
        f32::MIN,
        f32::MAX,
        f32::from_bits(1),           // smallest denormal
        f32::from_bits(0x8000_0001), // smallest negative denormal
    ];

    #[test]
    fn layout() {
        use core::mem::{align_of, size_of};
        assert_eq!(size_of::<M128F>(), 16);
        assert_eq!(align_of::<M128F>(), 16);
    }

    #[test]
    fn setters() {
        assert_eq!(setzero_ps(), M128F::zero());
        assert_eq!(bits_of(set1_ps(f32::NAN)), bits_of(M128F::splat(f32::NAN)));
        // C argument order: `set_ps(w, z, y, x)` → lane 0 gets `x`
        assert_eq!(
            set_ps(3.0, 2.0, 1.0, 0.0),
            M128F::from_lanes([0.0, 1.0, 2.0, 3.0])
        );
        assert_eq!(
            set_ps(0.0, 1.5, -2.5, 7.0),
            M128F {
                x: 7.0,
                y: -2.5,
                z: 1.5,
                w: 0.0
            }
        );
    }

    /// Lane mappings pinned from the C definitions (Intel intrinsics guide;
    /// the `matrix3_transpose`/`matrix4_transpose` idioms below cross-check
    /// them against the C `NO_INTRINSICS` reference and the conformance
    /// `math2` vectors).
    #[test]
    fn lane_mappings_golden() {
        let a = M128F::from_lanes([1.0, 2.0, 3.0, 4.0]);
        let b = M128F::from_lanes([5.0, 6.0, 7.0, 8.0]);
        assert_eq!(movelh_ps(a, b), M128F::from_lanes([1.0, 2.0, 5.0, 6.0]));
        assert_eq!(movehl_ps(a, b), M128F::from_lanes([7.0, 8.0, 3.0, 4.0]));
        assert_eq!(unpacklo_ps(a, b), M128F::from_lanes([1.0, 5.0, 2.0, 6.0]));
        assert_eq!(unpackhi_ps(a, b), M128F::from_lanes([3.0, 7.0, 4.0, 8.0]));
        // 0x55 (the dot idiom mask): every lane from lane 1
        assert_eq!(
            shuffle_ps::<0x55>(a, b),
            M128F::from_lanes([2.0, 2.0, 6.0, 6.0])
        );
        // mask 0 broadcasts lane 0; identity is _MM_SHUFFLE(1, 0, 1, 0) = 0x44
        assert_eq!(
            shuffle_ps::<0>(a, b),
            M128F::from_lanes([1.0, 1.0, 5.0, 5.0])
        );
        assert_eq!(
            shuffle_ps::<0x44>(a, b),
            M128F::from_lanes([1.0, 2.0, 5.0, 6.0])
        );
        // 0x99 = [a1, a2, b1, b2]
        assert_eq!(
            shuffle_ps::<0x99>(a, b),
            M128F::from_lanes([2.0, 3.0, 6.0, 7.0])
        );
    }

    /// The `matrix3_transpose` intrinsic path of `matrix3.c`, pinned to the
    /// conformance `math2/09-matrix3-transpose-w-lane` values (rows
    /// `1 4 7 0.3`, `2 5 8 0.3`, `3 6 9 0.3` transpose to columns
    /// `1 2 3 / 4 5 6 / 7 8 9`).
    #[test]
    fn matrix3_transpose_idiom() {
        let x = M128F::from_lanes([1.0, 4.0, 7.0, 0.3]);
        let y = M128F::from_lanes([2.0, 5.0, 8.0, 0.3]);
        let z = M128F::from_lanes([3.0, 6.0, 9.0, 0.3]);
        let tmp1 = movelh_ps(x, y);
        let tmp2 = movehl_ps(y, x);
        assert_eq!(tmp1, M128F::from_lanes([1.0, 4.0, 2.0, 5.0]));
        assert_eq!(tmp2, M128F::from_lanes([7.0, 0.3, 8.0, 0.3]));
        assert_eq!(
            shuffle_ps::<0xC8>(tmp1, z),
            M128F::from_lanes([1.0, 2.0, 3.0, 0.3])
        );
        assert_eq!(
            shuffle_ps::<0xDD>(tmp1, z),
            M128F::from_lanes([4.0, 5.0, 6.0, 0.3])
        );
        assert_eq!(
            shuffle_ps::<0xE8>(tmp2, z),
            M128F::from_lanes([7.0, 8.0, 9.0, 0.3])
        );
    }

    /// The `matrix4_transpose` intrinsic path of `matrix4.c`.
    #[test]
    fn matrix4_transpose_idiom() {
        let x = M128F::from_lanes([1.0, 2.0, 3.0, 4.0]);
        let y = M128F::from_lanes([5.0, 6.0, 7.0, 8.0]);
        let z = M128F::from_lanes([9.0, 10.0, 11.0, 12.0]);
        let t = M128F::from_lanes([13.0, 14.0, 15.0, 16.0]);
        let a0 = unpacklo_ps(x, z);
        let a1 = unpacklo_ps(y, t);
        let a2 = unpackhi_ps(x, z);
        let a3 = unpackhi_ps(y, t);
        assert_eq!(
            unpacklo_ps(a0, a1),
            M128F::from_lanes([1.0, 5.0, 9.0, 13.0])
        );
        assert_eq!(
            unpackhi_ps(a0, a1),
            M128F::from_lanes([2.0, 6.0, 10.0, 14.0])
        );
        assert_eq!(
            unpacklo_ps(a2, a3),
            M128F::from_lanes([3.0, 7.0, 11.0, 15.0])
        );
        assert_eq!(
            unpackhi_ps(a2, a3),
            M128F::from_lanes([4.0, 8.0, 12.0, 16.0])
        );
    }

    #[test]
    fn dot_golden() {
        let a = M128F::from_lanes([1.0, 2.0, 3.0, 4.0]);
        let b = M128F::from_lanes([5.0, 6.0, 7.0, 8.0]);
        assert_eq!(dot_ps(a, b), 70.0);
        // The C idiom's sum order: `(p0 + p2) + (p1 + p3)`
        let p = [5.0, 12.0, 21.0, 32.0];
        assert_eq!(dot_ps(a, b), (p[0] + p[2]) + (p[1] + p[3]));
    }

    /// P1-19.2 acceptance: the scalar fallback is bit-equal to the
    /// intrinsic, over random full `f32` bit patterns (NaN payloads,
    /// denormals, infs included).
    #[cfg_attr(miri, ignore)] // 110k intrinsic calls — real hardware only
    #[test]
    fn scalar_equals_intrinsic_random() {
        let mut rng = Rng(0x51ed);
        for _ in 0..10_000 {
            let (a, b) = random_pair(&mut rng);
            assert_arith_equal(add_ps(a, b), add_ps_scalar(a, b));
            assert_arith_equal(sub_ps(a, b), sub_ps_scalar(a, b));
            assert_arith_equal(mul_ps(a, b), mul_ps_scalar(a, b));
            assert_arith_equal(div_ps(a, b), div_ps_scalar(a, b));
            assert_bits_equal(min_ps(a, b), min_ps_scalar(a, b));
            assert_bits_equal(max_ps(a, b), max_ps_scalar(a, b));
            assert_bits_equal(movelh_ps(a, b), movelh_ps_scalar(a, b));
            assert_bits_equal(movehl_ps(a, b), movehl_ps_scalar(a, b));
            assert_bits_equal(unpacklo_ps(a, b), unpacklo_ps_scalar(a, b));
            assert_bits_equal(unpackhi_ps(a, b), unpackhi_ps_scalar(a, b));
            // `shuffle_ps` is const-generic (the C immediate form), so its
            // mask cannot be randomized here; the fixed-mask table test
            // below covers it against the scalar port on random values.
        }
    }

    /// `shuffle_ps` (const-generic) vs the scalar lane-selection port,
    /// over all masks used by the C code plus extra edge masks, with
    /// random lane values.
    #[test]
    fn shuffle_masks_against_scalar_random_values() {
        let mut rng = Rng(0x51ee);
        for _ in 0..1_000 {
            let (a, b) = random_pair(&mut rng);
            macro_rules! check {
                ($m:expr) => {
                    assert_bits_equal(shuffle_ps::<$m>(a, b), shuffle_ps_scalar(a, b, $m as u32));
                };
            }
            check!(0);
            check!(0x0F);
            check!(0x12);
            check!(0x44);
            check!(0x55);
            check!(0x99);
            check!(0xC8);
            check!(0xC9);
            check!(0xD2);
            check!(0xDD);
            check!(0xE8);
            check!(0xFF);
        }
    }

    /// Same acceptance on the curated edge set (cheap enough for Miri).
    #[test]
    fn scalar_equals_intrinsic_edges() {
        for &x in &EDGES {
            for &y in &EDGES {
                let a = M128F::splat(x);
                let b = M128F::splat(y);
                assert_arith_equal(add_ps(a, b), add_ps_scalar(a, b));
                assert_arith_equal(sub_ps(a, b), sub_ps_scalar(a, b));
                assert_arith_equal(mul_ps(a, b), mul_ps_scalar(a, b));
                assert_arith_equal(div_ps(a, b), div_ps_scalar(a, b));
                assert_bits_equal(min_ps(a, b), min_ps_scalar(a, b));
                assert_bits_equal(max_ps(a, b), max_ps_scalar(a, b));
            }
        }
    }

    /// min/max edge semantics pinned per the module docs: NaN handling and
    /// the deterministic ±0 results of `minps`/`maxps`.
    #[test]
    fn min_max_semantics() {
        let n = f32::NAN;
        // A NaN in either operand makes the ordered comparison false, so
        // the second operand is returned — on every architecture (the
        // hardware x86_64 result is QEMU-verified in the WP-35 CI round;
        // the Intel intrinsics guide's "the non-NaN operand is returned"
        // wording does not hold when the *second* operand is NaN).
        assert_eq!(
            min_ps_scalar(M128F::splat(n), M128F::splat(5.0)),
            M128F::splat(5.0)
        );
        // NaN results are compared by bits (NaN != NaN by value)
        assert_eq!(
            bits_of(min_ps_scalar(M128F::splat(5.0), M128F::splat(n))),
            [n.to_bits(); 4]
        );
        assert_eq!(
            max_ps_scalar(M128F::splat(n), M128F::splat(5.0)),
            M128F::splat(5.0)
        );
        assert_eq!(
            bits_of(max_ps_scalar(M128F::splat(5.0), M128F::splat(n))),
            [n.to_bits(); 4]
        );
        // -0.0 vs +0.0: `minps`/`maxps` return the second operand when the
        // ordered comparison is false (deterministic ±0).
        assert_eq!(
            min_ps_scalar(M128F::splat(-0.0), M128F::splat(0.0)),
            M128F::splat(0.0)
        );
        assert_eq!(
            min_ps_scalar(M128F::splat(0.0), M128F::splat(-0.0)),
            M128F::splat(-0.0)
        );
        assert_eq!(
            max_ps_scalar(M128F::splat(-0.0), M128F::splat(0.0)),
            M128F::splat(0.0)
        );
        assert_eq!(
            max_ps_scalar(M128F::splat(0.0), M128F::splat(-0.0)),
            M128F::splat(-0.0)
        );
    }

    /// `binop_slice`: bit-equal to the scalar reference on the lengths that
    /// cross the 8-lane (AVX2) and 4-lane (SSE2/NEON) boundaries, including
    /// NaN/±0 values straddling the boundaries.
    #[test]
    fn binop_slice_matches_scalar() {
        let ops = [
            BinOp::Add,
            BinOp::Sub,
            BinOp::Mul,
            BinOp::Div,
            BinOp::Min,
            BinOp::Max,
        ];
        for op in ops {
            for len in [0usize, 1, 3, 4, 7, 8, 9, 15, 16, 17, 33] {
                let mut rng = Rng(0x42 + len as u64);
                let a: Vec<f32> = (0..len).map(|_| rng.f32_bits()).collect();
                let b: Vec<f32> = (0..len).map(|_| rng.f32_bits()).collect();
                let want: Vec<f32> = a.iter().zip(&b).map(|(x, y)| op.scalar(*x, *y)).collect();
                let mut out = vec![f32::NAN; len];
                binop_slice(op, &a, &b, &mut out);
                match op {
                    BinOp::Min | BinOp::Max => {
                        // selection: exact bits (incl. NaN payloads)
                        assert_eq!(
                            out.iter().map(|x| x.to_bits()).collect::<Vec<_>>(),
                            want.iter().map(|x| x.to_bits()).collect::<Vec<_>>(),
                            "op {op:?} len {len}"
                        );
                    }
                    // producing ops: IEEE leaves produced-NaN payloads
                    // unspecified (see `assert_arith_equal`)
                    _ => {
                        for (k, (g, w)) in out.iter().zip(&want).enumerate() {
                            if g.is_nan() || w.is_nan() {
                                assert!(g.is_nan() && w.is_nan(), "op {op:?} len {len} k {k}");
                            } else {
                                assert_eq!(*g, *w, "op {op:?} len {len} k {k}");
                            }
                        }
                    }
                }
            }
        }
        // min/max with the documented edges straddling the 8/4 boundaries
        let edge8: Vec<f32> = EDGES[..8].to_vec();
        for op in [BinOp::Min, BinOp::Max] {
            let mut out = vec![f32::NAN; 8];
            binop_slice(op, &edge8, &edge8, &mut out);
            for (k, &x) in edge8.iter().enumerate() {
                assert_eq!(out[k], op.scalar(x, x), "same-value {op:?} lane {k}");
            }
        }
    }

    #[test]
    #[should_panic(expected = "equal length")]
    fn binop_slice_len_mismatch() {
        let a = [1.0f32, 2.0];
        let b = [1.0f32];
        let mut out = [0.0f32; 1];
        binop_slice(BinOp::Add, &a, &b, &mut out);
    }
}
