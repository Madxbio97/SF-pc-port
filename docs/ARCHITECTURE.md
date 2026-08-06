# Architecture

Syphon Filter PC is a hybrid runtime. The original R3000A program owns gameplay;
the Windows host owns presentation and platform services.

## Ownership contract

- Guest: mission state, actors, animation, combat, inventory, room state and game audio.
- Host: physical input, rendering, launcher/UI, windowing, saves and FMV playback.
- Bridges: host input becomes PAD state; guest output becomes immutable presentation
  snapshots and explicit checkpoint/FMV/mission commands.

Gameplay state has one writer. Presentation code may read typed bridge data but must
not write player position, combat, AI, inventory or animation.

## Build targets

| Target | Responsibility | May depend on |
| --- | --- | --- |
| `sf_core` | Errors, hashes and checked host-file I/O | Standard library |
| `sf_assets` | Bounded parsers for retail resource formats | `sf_core` |
| `sf_disc` | CUE/BIN, ISO9660 and raw sectors | `sf_core` |
| `sf_psx` | R3000A, GTE, bus, IRQ, DMA, CD-ROM, SPU and XA | `sf_core` |
| `sf_platform_input` | Platform-neutral input vocabulary | Standard library |
| `sf_game` | Campaign, guest VM, gameplay orchestration and bridge DTOs | Portable targets above |
| `sf_media` | Optional FFmpeg-backed STR decode | `sf_core`, FFmpeg |
| `sf_psycross_backend` | PsyCross/SDL/OpenGL/OpenAL presentation | `sf_game`, platform dependencies |
| `syphon_filter` | Shipping launcher and game executable | `sf_psycross_backend` |
| `sf_tool` | Development inspection/export commands | Portable targets |
| probes and tests | Deterministic and optional legal-ROM validation | Targets under test |

`sf_architecture_check` enforces the dependency direction and verifies that every
project-owned translation unit and `.inc` fragment belongs to a CMake target.

## Runtime clocks

- Retail gameplay publishes at 20 Hz.
- SPU, CD-ROM and timers advance independently on the recovered 120 Hz schedule.
- Presentation runs at the configured display rate and interpolates immutable guest
  snapshots without changing guest time.
- FMV decode and audio output are host services activated by explicit guest commands.

Snapshots include pending device events and guest presentation state. Host PCM queues
are transient and are cleared on restore rather than serialized.

## Presentation

`psycross_scene_viewer.cpp` is a small entry translation unit that includes ten focused
scene fragments. Together those fragments are still one large compilation and lifetime
unit; this is tracked in [REFACTOR_AUDIT.md](REFACTOR_AUDIT.md).

The backend owns:

- world/object projection, ordering tables, PGXP and depth state;
- VRAM residency, texture/CLUT uploads and native post-processing;
- HUD, pause-menu and optic presentation from guest snapshots;
- renderer-only interpolation, dynamic illumination and character shadows.

Static game geometry remains available to collision independently of renderer culling.
Optional effects must not mutate gameplay residency or guest object state.

## Performance contract

Release builds use interprocedural optimization when the toolchain supports it.
Performance work must preserve the 20 Hz guest clock and be measured with the procedure
in [PERFORMANCE.md](PERFORMANCE.md). Rendering fewer states, caching guest-invariant
work and reducing driver submissions are preferred before changing visual fidelity.

## Third-party boundary

The repository vendors a modified PsyCross tree under `external/PsyCross`; its license
and provenance are documented in [THIRD_PARTY.md](../THIRD_PARTY.md). No DuckStation
source tree is distributed by this repository.

## Validation

- Portable Release: `cmake --build --preset windows-release` and its CTest preset.
- Playable Release: `cmake --build --preset windows-psycross-release` and its CTest preset.
- Supported-ROM probes are opt-in and require an explicitly configured legal USA v1.1 image.
- Packaging, performance and manual gameplay checks are separate gates.
