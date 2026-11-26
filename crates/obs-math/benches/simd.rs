//! P1-19.3 benchmark code (delivered early; the subtask ticks after WP-41).
//!
//! Measures the P1-19.2 paths on the host: the dispatched 128-bit vector
//! ops (SSE2/AVX2 on x86_64, NEON on aarch64) against their scalar twins,
//! and the `binop_slice` batch path against the scalar loop. The
//! "not slower than C" comparison needs the WP-41 C baseline runner and
//! `benchmarks.md`; the C reference for these ops is the SSE2 `*_ps`
//! intrinsics of `libobs/graphics/{vec3,vec4,quat}.h` (128-bit, same width
//! as the SIMD path here — the batch path is the extra AVX2/NEON width the
//! C code does not have).
//!
//! Run: `cargo bench -p obs-math --bench simd`

use criterion::{Criterion, criterion_group, criterion_main};
use obs_math::simd::{
    BinOp, M128F, add_ps, add_ps_scalar, binop_slice, div_ps, div_ps_scalar, dot_ps, max_ps,
    max_ps_scalar, min_ps, min_ps_scalar, mul_ps, mul_ps_scalar, sub_ps, sub_ps_scalar,
};
use std::hint::black_box;

/// 2048 vectors per pass — one pass is well under the 16 KB L1D on the
/// relevant cores, so the benches measure the op, not the memory system.
const LANES: usize = 2048;
/// Batch length for `binop_slice` (8192 f32 = 1024 vectors).
const SLICE: usize = 8192;

/// Deterministic pseudo-random data (SplitMix64), no crate dependency.
fn make_vec(seed: u64, n: usize) -> Vec<M128F> {
    let mut z = seed;
    let mut next = move || {
        let mut w = z.wrapping_add(0x9E37_79B9_7F4A_7C15);
        w = (w ^ (w >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        w = (w ^ (w >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        z = w;
        f32::from_bits((w ^ (w >> 31)) as u32)
    };
    (0..n)
        .map(|_| M128F::from_lanes([next(), next(), next(), next()]))
        .collect()
}

fn make_slice(seed: u64, n: usize) -> Vec<f32> {
    make_vec(seed, n.div_ceil(4))
        .iter()
        .flat_map(|x| x.as_lanes())
        .take(n)
        .collect()
}

/// One 128-bit op over the whole buffer (independent ops, like the C
/// per-vec usage).
fn bench_128(c: &mut Criterion) {
    let (va, vb) = (make_vec(1, LANES), make_vec(2, LANES));
    let mut out = vec![M128F::zero(); LANES];
    /// (name, SIMD op, scalar op)
    type OpPair = (fn(M128F, M128F) -> M128F, fn(M128F, M128F) -> M128F);
    let ops: [(&str, OpPair); 6] = [
        ("add", (add_ps, add_ps_scalar)),
        ("sub", (sub_ps, sub_ps_scalar)),
        ("mul", (mul_ps, mul_ps_scalar)),
        ("div", (div_ps, div_ps_scalar)),
        ("min", (min_ps, min_ps_scalar)),
        ("max", (max_ps, max_ps_scalar)),
    ];
    let mut group = c.benchmark_group("simd-128");
    for (name, (sim, sca)) in ops {
        group.bench_function(format!("{name}/simd"), |bench| {
            bench.iter(|| {
                for i in 0..LANES {
                    out[i] = sim(black_box(va[i]), black_box(vb[i]));
                }
                out[0]
            })
        });
        group.bench_function(format!("{name}/scalar"), |bench| {
            bench.iter(|| {
                for i in 0..LANES {
                    out[i] = sca(black_box(va[i]), black_box(vb[i]));
                }
                out[0]
            })
        });
    }
    // The C dot idiom (mul + movehl + shuffle + add).
    group.bench_function("dot/simd", |bench| {
        bench.iter(|| {
            let mut acc = 0.0f32;
            for i in 0..LANES {
                acc += black_box(dot_ps(black_box(va[i]), black_box(vb[i])));
            }
            acc
        })
    });
    group.finish();
}

/// The batch path (`binop_slice`: AVX2/SSE2/scalar tails on x86_64,
/// NEON/scalar on aarch64) vs the scalar loop.
fn bench_slice(c: &mut Criterion) {
    let (fa, fb) = (make_slice(3, SLICE), make_slice(4, SLICE));
    let mut fo = vec![0.0f32; SLICE];
    let ops = [
        BinOp::Add,
        BinOp::Sub,
        BinOp::Mul,
        BinOp::Div,
        BinOp::Min,
        BinOp::Max,
    ];
    let mut group = c.benchmark_group("simd-slice");
    for op in ops {
        let name = format!("{op:?}");
        group.bench_function(format!("{name}/simd"), |bench| {
            bench.iter(|| {
                binop_slice(op, black_box(&fa), black_box(&fb), &mut fo);
                fo[0]
            })
        });
        group.bench_function(format!("{name}/scalar"), |bench| {
            bench.iter(|| {
                for k in 0..SLICE {
                    fo[k] = op.scalar(black_box(fa[k]), black_box(fb[k]));
                }
                fo[0]
            })
        });
    }
    group.finish();
}

criterion_group!(benches, bench_128, bench_slice);
criterion_main!(benches);
