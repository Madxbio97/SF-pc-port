# Roadmap

## Retail gameplay completion

- G2 — retail scripts, terrain triggers, spawn/despawn, doors, portals and
  scene transitions for all 20 missions. Complete; G2 ROM gates remain green.
- G3 — guest-authoritative AI, combat, stealth, targets, special actors,
  objectives, success and failure for all 20 missions. Complete; G3.2-G3.5
  natural-route, actor-lifecycle, AI/combat, stealth and overlay outcome gates
  are green and the gameplay test is accepted.
- G4 — connected campaign, FMV handoffs, checkpoints, saves, restarts and final
  stabilization of all 20 missions. Complete: transactional V3 saves with V1/V2
  migration, interruption-safe SOL/EOL ordering, final completion, exact
  checkpoint restart, explicit save-write recovery and a release `--game`
  entrypoint. The production-path ROM gate validates retail failure, checkpoint
  restore, success and staged EOL finalization for every mission; all 29 cataloged
  STR movies decode with video and audio.

## Hybrid migration

- H0 — guest ownership boundary: native input to PAD; retail gameplay/player/
  animation frames are authoritative. Complete.
- H1 — machine core: bus, IRQ, DMA, timers and deterministic scheduling around the
  existing interpreter/GTE. Complete.
- H2 — CD-ROM/DMA3 overlay loading and a resumable continuous guest loop. Complete.
- H3 — SPU MMIO/DMA4/IRQ9, CD-XA decode and bounded PCM output while keeping
  native FMV playback. Complete: strict builds/CTest and the retail gameplay-audio
  cadence/nonzero/checkpoint replay gate are green.
- H4 — renderer/UI command bridges and removal of remaining native gameplay paths.
  Complete.
- H5 — all mission overlays, snapshots, replay gates, native campaign/FMVs and
  campaign validation. Complete: 20/20 retail mission resources and all 13
  distinct overlays pass bootstrap, PCM, checkpoint and exact-replay gates.

The older native-port milestones below are retained as implementation history.

## M0: input and build foundation

- Validate the exact NTSC-U 1.1 executable.
- Read CUE/BIN and ISO9660 without external tools.
- Parse the PS-X EXE load contract.
- Keep all tests independent of copyrighted data.

## M1: executable map

- Generate a reproducible MIPS disassembly and function map using pinned Ghidra/PSX-loader versions.
- Identify PsyQ library functions and isolate them from game code.
- Catalog the 18 executable overlays in `BIN` and their load addresses.
- Add byte-accurate fixtures containing only project-owned metadata.

## M2: native bootstrap

- Bring up PsyCross through the backend boundary.
- Port initialization, title and menu modules.
- Load original TIM/HOG assets directly from the disc image.
- Stream the original startup STR/MDEC/XA directly from raw disc sectors.

## M3: vertical slice

- Mount and validate the first mission's original FOG/HOG package.
- Dispatch New Game through the Georgia Street opening sequence.
- Parse and render the original VLF/EMD terrain with PGXP texture correction.
- Port one mission's update/render/input loop.
- Add deterministic frame-state regression tests.
- Preserve original timing while allowing uncapped presentation.

## M4: content completion

- Port remaining missions and overlays.
- Complete remaining XA/STR playback paths and memory cards.
- Run static analysis, sanitizers and full automated regression suites.

Gameplay validation remains a user task.
