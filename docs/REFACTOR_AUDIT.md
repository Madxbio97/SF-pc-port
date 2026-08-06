# Refactor audit

Updated 2026-08-04. Counts are an inventory aid and may drift as code changes.

## Baseline

The project has strict compiler warnings, deterministic CTest targets and an architecture
dependency gate. No live `TODO`, `FIXME`, `HACK` or `XXX` markers were found. Vendored
PsyCross is excluded from project-owned style cleanup except for measured backend fixes.

## Current hotspots

| Area | Current shape | Risk | Next safe seam |
| --- | --- | --- | --- |
| Guest bridge reader | `LegacyGameplayVm::readBridgeState` is about 3,264 lines with hundreds of RAM reads and failure exits | High | Characterize fault stages, then split readers by bridge record |
| Gameplay session | Constructor about 539 lines; bridge and resident-object sync functions about 500 lines each | Medium | Extract loaders and immutable snapshot assembly |
| Checkpoints | 52 parallel `checkpoint_*` fields and roughly 100 manual capture/restore assignments | Medium | Introduce one `HostCheckpointState` with paired capture/restore tests |
| Scene renderer | 168-line entry includes ten `.inc` files totaling about 15,875 lines | High | Measure and extract ownership units, not arbitrary file slices |
| HMD presentation | World posing is single-pass, but attachments, callouts and optic capture can still pose an actor again | Medium | Share a display-frame pose cache across presentation consumers |
| Tests | `r3000_runtime_tests.cpp` and `test_main.cpp` dominate test source size | Low | Split by subsystem while preserving fixtures and CTest isolation |
| ROM probes | Repeated bootstrap/tick/wait/replay scaffolding across probe executables | Low | Extract `apps/probe_support` |

Process-global renderer state and cached pointers into model variants remain lifetime
risks. The current containers are stable in practice, but future extraction should make
those invariants explicit.

## Changes in this revision

- Prepared retail vertex-light matrices once per presented light instead of once per
  illuminated vertex.
- Removed the second full `SceneObject` copy from presentation interpolation.
- Enabled optional Release IPO/LTO.
- Skipped zero-vertex PsyCross VBO uploads and empty log flushes.
- Fused HMD pose resolution, ground-plane evaluation and world transformation into
  one model-vertex pass; immutable referenced-vertex masks are cached per model.
- Skipped clearing and submitting inactive SCRIM, SVD and scanner ordering tables.
- Replaced double actor-shadow OT replay with one stencil-gated pass while preserving
  single darkening at overlapping model triangles.
- Reused authoritative `SceneObject` storage at the 20 Hz capture rate and updated only
  interpolated transforms and active HMD bones on display frames.
- Recorded immutable vertex formats once per PsyCross VAO instead of issuing the same
  attribute setup calls on every non-empty OT upload.
- Materialized static-world `SVECTOR` values only when clipping, fallback projection or a
  lighting-cache miss actually needs them.
- Made `GameplaySession` non-copyable/non-movable and rejected temporary mission packages.
- Added a reproducible 240 FPS validation contract and a current documentation index.
- Gated fixed optic packets by the live guest aim mode and synchronized the ROM probe with
  legal five-word `POLY_F4` packets.

## Ordered follow-up

1. Add per-stage CPU timers and counters for snapshot interpolation, pose work, terrain,
   OT parsing/upload, HUD and swap.
2. Share a display-frame HMD pose cache with attachments, callouts and optic captures;
   precompute any remaining immutable model metadata.
3. Track explicit fire and guest-overlay pass activity so empty specialized OTs skip
   both clearing and submission.
4. Measure static-world projection and PGXP cache pressure before persistent GPU batches.
5. Consolidate checkpoint state and split bridge decoding behind characterization tests.
6. Consider a GPU static-world path only after the lower-risk CPU/driver work is measured.

Large functions are not split mechanically: guest fault ordering, renderer pointer
stability and snapshot lifetimes require tests before ownership moves.
