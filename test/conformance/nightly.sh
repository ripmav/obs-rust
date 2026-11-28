#!/usr/bin/env bash
# P1-30.1 (WP-44): the OBS_ENABLE_RUST soak run — both modes of the
# OBS_ENABLE_RUST pair as one comparable nightly artifact. The contract
# (start date, what is watched, the stop/flip condition) is in
# docs/rust-rewrite/soak.md.
#
#   test/conformance/nightly.sh
#
# Runs `run.sh check` (OBS_ENABLE_RUST=OFF, the all-C reference) and
# `run.sh swap` (OBS_ENABLE_RUST=ON — the C sources the D3 patch
# test/conformance/obs-enable-rust.patch excludes are dropped, their
# symbols come from target/release/libobs_ffi.a) over every CI area
# (ci-areas.txt — the same selection the conformance job runs; areas
# needing dependencies the CI does not install are excluded there). The
# D3 patch is expected to be applied to the linked obs-studio checkout
# already (the nightly workflow does that); the patch state is recorded
# in the summary.
#
# Outputs (under target/nightly/, uploaded as the soak artifact):
#   check.log    full `run.sh check` log
#   swap.log     full `run.sh swap` log
#   summary.txt  the comparable per-run summary: run stamp, repo HEAD,
#                obs-studio pin, D3 patch state, and per mode the exit
#                status, ok/skip/fail counts, every FAIL line and the
#                crash-signature lines. Diff the summary of two
#                consecutive runs to spot a new crash signature or a new
#                failure.
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="$root/target/nightly"
mkdir -p "$out"

# The obs-studio checkout behind the repo-root libobs link (works both in
# CI, where the checkout lands in obs-studio/, and locally, where the link
# points straight at the sibling checkout).
obs_root="missing"
if [ -e "$root/libobs" ]; then
	obs_root="$(cd -P "$root/libobs/.." 2>/dev/null && pwd || echo missing)"
fi
patch_state="not applied"
if [ "$obs_root" != missing ] && grep -q "OBS_ENABLE_RUST" "$obs_root/libobs/CMakeLists.txt" 2>/dev/null; then
	patch_state="applied"
fi

# Crash signatures: bash's fatal job message for a process killed by a
# signal (`<invocation>: line N:  PID signal ...` — the prefix is the
# invocation path, e.g. `./run.sh`, or `/bin/bash` when the shell itself
# dies; committed vectors may legitimately contain signal text, e.g.
# rtmpstream's `probe crash signal=SIGSEGV`, so signal words alone are
# not used). A crash also shows as a non-zero exit without FAIL lines
# (set -e aborts the run).
crash_re='^(\S+): line [0-9]+: +[0-9]+ '

summary="$out/summary.txt"
areas_file="$here/ci-areas.txt"
status=0
for mode in check swap; do
	if [ "$mode" = swap ]; then label=ON; else label=OFF; fi
	# Per-area build dirs under $RUNNER_TEMP in CI (not the /tmp mktemp
	# default) so the runners cannot be starved by a small /tmp (same rule
	# as the conformance job), which also runs per area.
	builddir="${RUNNER_TEMP:-$out}/build-$mode"
	mkdir -p "$builddir"
	: >"$out/$mode.log"
	rc=0
	for a in $(grep -vE '^[[:space:]]*(#|$)' "$areas_file"); do
		mkdir -p "$builddir/$a"
		(cd "$here" && CONFORMANCE_BUILD_DIR="$builddir/$a" ./run.sh "$mode" "$a") >>"$out/$mode.log" 2>&1 || rc=1
	done
	[ "$rc" -eq 0 ] || status=1
	{
		echo "mode: OBS_ENABLE_RUST=$label (run.sh $mode)"
		echo "  exit: $rc"
		echo "  ok: $(grep -c '^ok ' "$out/$mode.log")  skip: $(grep -c '^skip ' "$out/$mode.log")  fail: $(grep -c '^FAIL ' "$out/$mode.log")"
		echo "  fail:"
		if grep -q '^FAIL ' "$out/$mode.log"; then
			grep '^FAIL ' "$out/$mode.log" | sort -u | sed 's/^/    /'
		else
			echo "    (none)"
		fi
		echo "  crash:"
		if grep -qE "$crash_re" "$out/$mode.log"; then
			grep -E "$crash_re" "$out/$mode.log" | sort -u | sed 's/^/    /'
		else
			echo "    (none)"
		fi
	} >>"$summary"
done

{
	echo "# obs-rust nightly soak summary (P1-30.1) — comparable per run; diff"
	echo "# consecutive runs for new crash signatures or new failures."
	echo "run: $(date -u +%Y-%m-%dT%H:%M:%SZ)${GITHUB_RUN_ID:+ (github run $GITHUB_RUN_ID)}"
	echo "repo: $(git -C "$root" rev-parse HEAD 2>/dev/null || echo unknown)"
	echo "obs-studio: $(git -C "$obs_root" rev-parse HEAD 2>/dev/null || echo unknown) ($(git -C "$obs_root" describe --tags --exact-match 2>/dev/null || echo unknown))"
	echo "d3 patch: $patch_state (test/conformance/obs-enable-rust.patch)"
	echo "overall: $([ "$status" -eq 0 ] && echo ok || echo FAILED)"
} >"$out/summary.header"
cat "$out/summary.header" "$summary" >"$summary.tmp"
mv "$summary.tmp" "$summary"
rm -f "$out/summary.header"

cat "$summary"
exit "$status"
