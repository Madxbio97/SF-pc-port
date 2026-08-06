# Roadmap

Completed recovery milestones are retained in [STAGES.md](STAGES.md) and the
`*_FINDINGS.md` files. This page lists active work only.

## Performance and presentation

- Establish three repeatable scene captures and record a hardware baseline using
  [PERFORMANCE.md](PERFORMANCE.md).
- Add frame-stage timings and OT/vertex/upload counters without enabling them by default.
- Reuse presentation object storage between guest publications.
- Skip inactive optional ordering tables and eliminate zero-work driver submissions.
- Share HMD pose/anchor results across HUD and world rendering.
- Keep 20 Hz gameplay, 120 Hz audio/device timing and current visual output unchanged.

Success: the declared reference machine sustains a 4.17 ms 95th-percentile frame
budget in all reference scenes, with no rendering, audio or gameplay regression.

## Code structure

- Consolidate parallel checkpoint fields into one tested host checkpoint value.
- Split guest bridge decoding by record type after fault-stage characterization tests.
- Extract common ROM-probe bootstrap/replay support.
- Split the two test monoliths into independently reported subsystem targets.
- Reduce process-global renderer state as ownership seams are established.

## Renderer architecture

- Precompute immutable HMD metadata at asset load.
- Batch repeated OT/VBO submissions and replay shadow geometry without reparsing it.
- Evaluate a GPU static-world transform path only after CPU/driver counters show that
  lower-risk work is exhausted.

## Quality gates

- Keep portable and PsyCross Release CTest suites green.
- Keep architecture and release-package validation mandatory.
- Add sanitizer/static-analysis presets suitable for CI.
- Treat supported-ROM probes and manual gameplay captures as explicit, documented gates.
