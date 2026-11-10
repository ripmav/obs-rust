# Test matrix: abi-conformance

- Test file: `test/abi-host/abi-host.c` + `test/test-input/` module (P0-15)
- Task: P0-15 (blocked on the reference C checkout; cases specified here from
  the subtask file so the test lands to this matrix)
- Golden files: `test/abi-host/traces/*.golden`

| Case | Input | Expected | Platforms | Status |
|---|---|---|---|---|
| module load | `obs_open_module`/`obs_init_module` on the test module | `obs_module_load` runs; source, filter, encoder, output, service all registered | Linux, Windows, macOS | pending (P0-15) |
| source lifecycle | create one object of each registered kind | callback trace `(callback, thread id, order)` in order create → update → video_tick ×10 → video_render ×10 → destroy | Linux, Windows, macOS | pending (P0-15) |
| get_properties | request properties after update | properties callback fires once, after update | Linux, Windows, macOS | pending (P0-15) |
| filter audio | audio callback through the filter chain | `filter_audio` in trace, per audio callback | Linux, Windows, macOS | pending (P0-15) |
| trace goldens | 10 ticks of the driven scene | trace matches `traces/*.golden` byte-for-byte (stable across runs) | Linux, Windows, macOS | pending (P0-15) |
| struct size tolerance | plugin compiled against an older (smaller) `obs_source_info` | module accepted (loader `memcpy`s the caller's `sizeof`); no warning | Linux, Windows, macOS | pending (P0-15) |
| CI both modes | `ctest -R abi` with `OBS_ENABLE_RUST` ON and OFF | both green | Linux, Windows, macOS | pending (P0-15) |

Notes:
- Trace lines are `(callback name, thread id, order)` appended to a text file
  and compared against goldens (P0-15.3/.4).
- The struct-size case is the mechanic ADR 0002 pins: appending fields at the
  end of `obs_source_info` must keep old plugins loading.
- All rows become `pass` when P0-15 lands (needs the C checkout linked via
  `tools/link-obs-source.sh` and the CI job from P0-09).
