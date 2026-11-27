//! P0-17 Rust-side workloads (WP-41): criterion benches + canonical-JSON
//! gate output.
//!
//! `matmul4` — 1M dependent `matrix4_mul` (the ported P1-16 kernel). The C
//! baseline is the pinned `libobs/graphics/matrix4.c` running the same
//! affine rotation+translation chain (test/ref-bench/ref-bench.c).
//!
//! `scene_render` — the headless 20-item scene **stand-in** (documented
//! deviation, P0-17.3): it reproduces the per-item CPU math of the 33.0.1
//! scene renderer over the ported obs-math — the `set_item_transform`
//! sequence (obs-scene.c:657-661: identity, scale3f, translate3f(-origin),
//! rotate_aa4f(0,0,1,RAD(rot)), translate3f(pos)) and the per-frame
//! `gs_matrix_mul(&item->draw_transform)` (obs-scene.c:964). The full C
//! path needs the graphics runtime (texrender/shaders), so it has no C
//! baseline in this runner.
//!
//! The pixel (NV12→RGB) and audio-mix Rust benches land with the Phase 2
//! obs-media-io ports (`crates/obs-media-io/benches/`); the C baselines
//! for both are committed now (test/ref-bench + docs/rust-rewrite/benchmarks.md).
//!
//! Run:
//!   cargo bench -p obs-math --bench p017              criterion report
//!   cargo bench -p obs-math --bench p017 -- --gate    canonical JSON (schema 1,
//!                                                      the regression-gate input)

use obs_math::{
    Matrix4, axisang_set, matrix4_from_axisang, matrix4_identity, matrix4_mul, matrix4_rotate_aa4f,
    matrix4_scale3f, matrix4_translate3f,
};
use std::hint::black_box;
use std::time::Instant;

/// P0-17.2: 1M matrix multiplies (dependent chain, so the loop cannot be
/// vectorised across iterations — both C and Rust measure the op itself).
const MMUL_ITERS: u32 = 1_000_000;
/// P0-17.3: a 20-item scene.
const ITEMS: usize = 20;
/// Gate mode: fixed runs, same default as the C runner.
const GATE_RUNS: usize = 30;
/// Gate mode: untimed runs before the timed ones.
const GATE_WARMUP: usize = 2;

/// The matmul4 workload matrix: bounded affine (rotation 1.0 rad about
/// (0.3, 0.5, 0.7) + small translation) — powers stay bounded, so the
/// 1M-deep dependent chain never overflows f32. Identical to the C runner.
fn mul_a() -> Matrix4 {
    let mut m = matrix4_identity();
    matrix4_from_axisang(&mut m, &axisang_set(0.3, 0.5, 0.7, 1.0));
    m.t.x = 0.1;
    m.t.y = 0.2;
    m.t.z = 0.05;
    m
}

/// One C reference item (obs-scene.c, 33.0.1): deterministic per-index
/// position/origin/rotation/scale.
fn item_transform(i: usize) -> Matrix4 {
    let pos = (100.0 * i as f32, 50.0 * i as f32);
    let origin = (320.0f32, 180.0f32);
    let rot_deg = 7.5f32 * i as f32;
    let scale = (0.95f32 + 0.01 * i as f32, 1.05f32 - 0.01 * i as f32);
    // C passes (dst, src) = (t, t); Rust takes a snapshot (Copy) per call.
    let mut t = matrix4_identity();
    let s = t;
    matrix4_scale3f(&mut t, &s, scale.0, scale.1, 1.0);
    let s = t;
    matrix4_translate3f(&mut t, &s, -origin.0, -origin.1, 0.0);
    let s = t;
    matrix4_rotate_aa4f(&mut t, &s, 0.0, 0.0, 1.0, rot_deg.to_radians());
    let s = t;
    matrix4_translate3f(&mut t, &s, pos.0, pos.1, 0.0);
    t
}

/// One headless scene frame: the per-item CPU math of the 20 items.
fn scene_frame(scene: &Matrix4) -> f32 {
    let mut sink = 0.0f32;
    for i in 0..ITEMS {
        let t = item_transform(i);
        let mut m = matrix4_identity();
        matrix4_mul(&mut m, scene, &t); // gs_matrix_mul(&item->draw_transform)
        sink += m.t.w;
    }
    sink
}

fn bench_matmul(c: &mut criterion::Criterion) {
    let a = mul_a();
    let mut m = a;
    c.bench_function("matmul4", |bench| {
        bench.iter(|| {
            for _ in 0..MMUL_ITERS {
                // C passes (dst, m1) = (m, m); Rust takes a Copy snapshot.
                let s = m;
                matrix4_mul(&mut m, &s, &a);
            }
            black_box(m.t.w)
        })
    });
}

fn bench_scene(c: &mut criterion::Criterion) {
    let scene = matrix4_identity(); // the current gs matrix
    c.bench_function("scene_render", |bench| {
        bench.iter(|| black_box(scene_frame(&scene)))
    });
}

/// Median of `GATE_RUNS` timed runs, in ns per work unit (same contract as
/// the C runner; the chain continues across runs exactly like in C).
fn gate_median(ns_per_run: &[f64]) -> f64 {
    let mut s = ns_per_run.to_vec();
    s.sort_by(f64::total_cmp);
    s[GATE_RUNS / 2]
}

fn gate() {
    let a = mul_a();
    let mut m = a;
    for _ in 0..GATE_WARMUP {
        for _ in 0..MMUL_ITERS {
            let s = m;
            matrix4_mul(&mut m, &s, &a);
        }
    }
    let mut mmul = Vec::with_capacity(GATE_RUNS);
    let mut sink = 0.0f32;
    for _ in 0..GATE_RUNS {
        let t = Instant::now();
        for _ in 0..MMUL_ITERS {
            let s = m;
            matrix4_mul(&mut m, &s, &a);
        }
        sink += m.t.w;
        mmul.push(t.elapsed().as_nanos() as f64);
    }
    let _ = black_box(sink);

    let scene = matrix4_identity();
    for _ in 0..GATE_WARMUP {
        black_box(scene_frame(&scene));
    }
    let mut scene_times = Vec::with_capacity(GATE_RUNS);
    for _ in 0..GATE_RUNS {
        let t = Instant::now();
        black_box(scene_frame(&scene));
        scene_times.push(t.elapsed().as_nanos() as f64);
    }

    println!("{{");
    println!("  \"schema\": 1,");
    println!("  \"side\": \"rust\",");
    println!("  \"os\": \"{}\",", std::env::consts::OS);
    println!("  \"arch\": \"{}\",", std::env::consts::ARCH);
    println!("  \"cpu\": \"{}\",", cpu_model());
    println!("  \"benches\": {{");
    println!(
        "    \"matmul4\": {{\"iters\": {}, \"runs\": {}, \"ns_per_op\": {}}},",
        MMUL_ITERS,
        GATE_RUNS,
        fmt_g9(gate_median(&mmul) / MMUL_ITERS as f64)
    );
    println!(
        "    \"scene_render\": {{\"iters\": 1, \"runs\": {}, \"ns_per_op\": {}}}",
        GATE_RUNS,
        fmt_g9(gate_median(&scene_times))
    );
    println!("  }}");
    println!("}}");
}

/// `%.9g`-style rendering (9 significant digits, valid JSON) — keeps the
/// Rust side of the canonical JSON as close to the C runner's `%.9g` as
/// the Rust format syntax allows.
fn fmt_g9(v: f64) -> String {
    format!("{:.9e}", v)
}

/// First "model name" (x86_64) / "Model" / "Hardware" line of
/// /proc/cpuinfo (linux); "unknown" elsewhere — same heuristic as the C
/// runner, so both sides of the JSON agree on the machine label.
fn cpu_model() -> String {
    #[cfg(target_os = "linux")]
    {
        if let Ok(data) = std::fs::read_to_string("/proc/cpuinfo") {
            for line in data.lines() {
                let key = if line.starts_with("model name") {
                    Some("model name")
                } else if line.starts_with("Model") {
                    Some("Model")
                } else {
                    None
                };
                let Some(key) = key else { continue };
                let Some(rest) = line.get(key.len()..) else {
                    continue;
                };
                let Some(val) = rest.split_once(':').map(|(_, v)| v.trim()) else {
                    continue;
                };
                return val.to_owned();
            }
        }
    }
    "unknown".to_owned()
}

fn main() {
    if std::env::args().skip(1).any(|a| a == "--gate") {
        gate();
        return;
    }
    let mut c = criterion::Criterion::default();
    bench_matmul(&mut c);
    bench_scene(&mut c);
}
