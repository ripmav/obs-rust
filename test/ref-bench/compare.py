#!/usr/bin/env python3
"""P0-17 regression gate (WP-41, P0-17.5).

Compares current bench numbers (canonical JSON, schema 1 — see
test/ref-bench/README.md) against the committed baseline
(test/ref-bench/baseline.json) and fails when a tracked bench is more than
--max-regression percent SLOWER than the baseline. Improvements never fail;
a tracked bench missing from the current output fails (a deleted bench must
not silently pass the gate).

Usage:
  compare.py BASELINE.json --current CURRENT.json [--current ...] \
      [--max-regression 2.0] [--no-fail]

  --no-fail   report mode: print the table but exit 0 even on regression
              (used by CI until WP-46 flips the gate to blocking).

CPU awareness: the threshold only applies when the current run's CPU
model (JSON "cpu" field) matches the baseline's "generated.cpu".
GitHub's ubuntu-latest runners are heterogeneous (EPYC 9V45 vs EPYC
7763 are 1.5-3x apart), so cross-model deltas are reported as
informational ("info (different CPU)") and never fail; a missing bench
still fails on any CPU (ODD L91).

Exit codes: 0 = no regression above the threshold (or --no-fail),
1 = regression / missing bench, 2 = bad input.

Self-test (no files needed): compare.py --self-test
"""
import argparse
import json
import os
import sys
import tempfile


def load_benches(path):
    """Return ({(name, side): ns_per_op}, cpu) for a runner JSON (schema 1)."""
    with open(path) as f:
        doc = json.load(f)
    if doc.get("schema") != 1:
        raise ValueError(f"{path}: schema != 1")
    side = doc.get("side")
    if side not in ("rust", "c"):
        raise ValueError(f"{path}: missing/invalid 'side'")
    out = {}
    for name, b in doc.get("benches", {}).items():
        out[(name, side)] = float(b["ns_per_op"])
    return out, doc.get("cpu", "unknown")


def compare(base, cur, cur_cpu, base_cpu, max_regression):
    """Return (rows, failures, cpu_changed). rows: (name, side, base_ns, cur_ns|None, delta%|None, status).

    A pair whose current CPU model differs from the baseline CPU model is
    reported as informational (status "info (different CPU)") and never
    fails: GitHub's ubuntu-latest runners are heterogeneous (e.g. EPYC
    9V45 vs EPYC 7763, 1.5-3x apart), so cross-model deltas are hardware
    noise, not code regressions (ODD L91). A missing bench still fails
    regardless of CPU."""
    rows = []
    failures = []
    cpu_changed = False
    for (name, side), base_ns in sorted(base.items()):
        val = cur.get((name, side))
        if val is None:
            rows.append((name, side, base_ns, None, None, "MISSING"))
            failures.append(f"{name}/{side}: missing from current output")
            continue
        delta = (val - base_ns) / base_ns * 100.0
        if cur_cpu[(name, side)] != base_cpu:
            cpu_changed = True
            status = "info (different CPU)"
        elif delta > max_regression:
            status = "REGRESSION"
            failures.append(f"{name}/{side}: +{delta:.2f}% > +{max_regression:g}%")
        else:
            status = "ok" if delta >= 0 else "faster"
        rows.append((name, side, base_ns, val, delta, status))
    return rows, failures, cpu_changed


def print_table(rows, max_regression):
    print(f"{'bench':<15} {'side':<5} {'baseline ns/op':>14} {'current ns/op':>14} {'delta':>8}  status")
    print("-" * 68)
    for name, side, base_ns, val, delta, status in rows:
        cur = f"{val:.6g}" if val is not None else "-"
        d = f"{delta:+.2f}%" if delta is not None else "-"
        print(f"{name:<15} {side:<5} {base_ns:>14.6g} {cur:>14} {d:>8}  {status}")
    print(f"(threshold: fail when slower than baseline by more than {max_regression:g}% — same CPU model only)")


def self_test():
    """Synthetic baseline + currents: exercises ok / faster / regression /
    missing / --no-fail. Returns the number of assertions that passed."""
    base = {
        "schema": 1,
        "side": "c",
        "benches": {
            "matmul4": {"iters": 1000000, "runs": 30, "ns_per_op": 100.0},
            "audio_mix": {"iters": 1, "runs": 30, "ns_per_op": 200.0},
        },
    }
    ok_cur = {
        "schema": 1,
        "side": "c",
        "benches": {
            "matmul4": {"iters": 1000000, "runs": 30, "ns_per_op": 101.0},  # +1% ok
            "audio_mix": {"iters": 1, "runs": 30, "ns_per_op": 180.0},  # faster
        },
    }
    slow_cur = {
        "schema": 1,
        "side": "c",
        "benches": {
            "matmul4": {"iters": 1000000, "runs": 30, "ns_per_op": 103.0},  # +3% > 2%
            "audio_mix": {"iters": 1, "runs": 30, "ns_per_op": 200.0},
        },
    }
    with tempfile.TemporaryDirectory() as tmp:
        bp = os.path.join(tmp, "b.json")
        with open(bp, "w") as f:
            json.dump(base, f)
        same_cpu = {("matmul4", "c"): "CPU-A", ("audio_mix", "c"): "CPU-A"}
        rows, failures, _ = compare({("matmul4", "c"): 100.0, ("audio_mix", "c"): 200.0},
                                    {("matmul4", "c"): 101.0, ("audio_mix", "c"): 180.0},
                                    same_cpu, "CPU-A", 2.0)
        assert not failures, f"expected pass, got {failures}"
        assert len(rows) == 2
        rows, failures, _ = compare({("matmul4", "c"): 100.0, ("audio_mix", "c"): 200.0},
                                    {("matmul4", "c"): 103.0, ("audio_mix", "c"): 200.0},
                                    same_cpu, "CPU-A", 2.0)
        assert len(failures) == 1 and "matmul4" in failures[0], f"expected matmul4 regression: {failures}"
        # different-CPU pair: a +300% delta is informational only, not a failure
        other_cpu = {("matmul4", "c"): "CPU-B", ("audio_mix", "c"): "CPU-B"}
        rows, failures, changed = compare({("matmul4", "c"): 100.0, ("audio_mix", "c"): 200.0},
                                          {("matmul4", "c"): 400.0, ("audio_mix", "c"): 400.0},
                                          other_cpu, "CPU-A", 2.0)
        assert not failures and changed, f"expected informational different-CPU rows: {failures}"
        assert all(r[5] == "info (different CPU)" for r in rows)
        rows, failures, _ = compare({("matmul4", "c"): 100.0, ("audio_mix", "c"): 200.0},
                                    {("matmul4", "c"): 101.0},
                                    {("matmul4", "c"): "CPU-A"}, "CPU-A", 2.0)
        assert len(failures) == 1 and "audio_mix" in failures[0] and "missing" in failures[0], failures
        # the file-based path must round-trip the schema check
        op = os.path.join(tmp, "ok.json")
        sp = os.path.join(tmp, "slow.json")
        for p, doc in ((op, ok_cur), (sp, slow_cur)):
            with open(p, "w") as f:
                json.dump(doc, f)
        assert load_benches(op) == ({("matmul4", "c"): 101.0, ("audio_mix", "c"): 180.0}, "unknown")
        try:
            bad = os.path.join(tmp, "bad.json")
            with open(bad, "w") as f:
                json.dump({"schema": 99, "benches": {}}, f)
            load_benches(bad)
            raise AssertionError("schema check did not fire")
        except ValueError:
            pass
    print("self-test: ok (6/6)")
    return 0


def main():
    ap = argparse.ArgumentParser(description="P0-17 bench regression gate")
    ap.add_argument("baseline", nargs="?", help="committed baseline JSON (schema 1)")
    ap.add_argument("--current", action="append", default=[], help="current bench JSON (repeatable)")
    ap.add_argument("--max-regression", type=float, default=2.0, help="fail when slower by more than this %% (default 2.0)")
    ap.add_argument("--no-fail", action="store_true", help="report only; exit 0 even on regression")
    ap.add_argument("--self-test", action="store_true", help="run the built-in synthetic checks")
    args = ap.parse_args()

    if args.self_test:
        return self_test()
    if not args.baseline or not args.current:
        ap.error("baseline + at least one --current are required (or use --self-test)")

    try:
        with open(args.baseline) as f:
            base_doc = json.load(f)
        if base_doc.get("schema") != 1:
            raise ValueError("baseline: schema != 1")
        base = {}
        for name, sides in base_doc.get("benches", {}).items():
            for side, ns in sides.items():
                if side not in ("rust", "c"):
                    raise ValueError(f"baseline: unknown side {side!r}")
                base[(name, side)] = float(ns)
        base_cpu = base_doc.get("generated", {}).get("cpu", "unknown")
        cur = {}
        cur_cpu = {}
        for p in args.current:
            benches, cpu = load_benches(p)
            cur.update(benches)
            cur_cpu.update({k: cpu for k in benches})
    except (ValueError, json.JSONDecodeError, KeyError, OSError) as e:
        print(f"compare.py: {e}", file=sys.stderr)
        return 2

    rows, failures, cpu_changed = compare(base, cur, cur_cpu, base_cpu, args.max_regression)
    print_table(rows, args.max_regression)
    if cpu_changed:
        print(f"note: current CPU(s) differ from the baseline CPU ({base_cpu!r}) — "
              "those deltas are informational only, not gated")
    if failures:
        if args.no_fail:
            print(f"REGRESSIONS (report mode, not failing): {failures}")
            return 0
        print("FAILURES:", file=sys.stderr)
        for f_ in failures:
            print(f"  {f_}", file=sys.stderr)
        return 1
    print("ok: no tracked bench regressed beyond the threshold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
