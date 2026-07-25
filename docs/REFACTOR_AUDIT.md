# Refactor audit

## Scope

This audit covers project-owned C++, CMake targets, tests and release-facing
tooling. Vendored PsyCross code is intentionally excluded so upstream history
and license boundaries remain intact.

## Completed cleanup

- Removed the rejected filtered-TIM remaster runtime, generated assets and
  release-copy path. The build no longer depends on derived retail resources.
- Moved TIM extraction from the 4,000+ line `sf_tool` command file into the
  dedicated `apps/sf_tool/ui_export` module.
- Centralized checked binary host-file reads/writes in `sf_core`; extraction and
  raw-RAM diagnostics no longer maintain separate I/O implementations.
- Moved mouse capture, swap-interval lifetime and fixed presentation pacing out
  of the 13,000-line scene renderer into `psycross_runtime_guards`.
- Moved the retail MENU.OVL map tables/projections and pause-menu data assembly
  out of the renderer into `sf_game`; coordinate behavior now has dedicated
  characterization tests.
- Moved VLF validation, physical-page mapping and low-level TIM/VRAM uploads
  into `psycross_vram`, with layout and error-path tests.
- Removed the unused objective-name heuristic and scene-target search path that
  had no callers after retail map records became authoritative.
- Added an architecture CTest gate. Portable layers cannot acquire SDL,
  PsyCross/OpenGL/OpenAL or upward project dependencies, and every project-owned
  translation unit must belong to an explicit CMake target.
- Added binary-I/O round-trip and error-path coverage.
- Centralized collision-safe temporary-directory ownership for file-system
  tests, so parallel CTest runs cannot share or leak scratch state.
- Restricted the architecture scanner to actual include directives; comments
  and diagnostic text can no longer create false dependency violations.
- Kept generated Python caches and local scratch output outside source control.

## Current ownership boundaries

- `sf_core`: error handling, hashes and host-neutral file utilities.
- `sf_assets`, `sf_disc`, `sf_psx`: bounded parsers and hardware/runtime models;
  no presentation dependencies.
- `sf_game`: guest-authoritative campaign and gameplay orchestration.
- `sf_platform_input`: host input vocabulary without SDL ownership.
- `sf_psycross_backend`: SDL/PsyCross/OpenGL/OpenAL presentation only.
- `sf_tool`: command dispatch and diagnostics; resource export lives in its own
  module and never enters the playable target.

## Remaining large modules

The following files are large but live; deleting or mechanically splitting them
would change ABI/state-lifetime assumptions and therefore requires dedicated
characterization tests first:

- `psycross_scene_viewer.cpp`: raw VRAM transport is now isolated; the next safe
  seams are residency policy, world submission and HUD/pause drawing.
- `legacy_gameplay_vm.cpp`: next seams are HLE dispatch, snapshot serialization
  and guest-frame execution.
- `gameplay.cpp`: next seams are guest snapshot ingestion, presentation state and
  mission/session lifecycle.

New code must not expand these files with unrelated responsibilities. The
architecture gate prevents layer regressions while those seams are extracted in
behavior-preserving passes.
