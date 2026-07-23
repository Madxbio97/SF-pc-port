# Work stages and test gates

> Historical native-port plan. It is retained as an implementation record; the
> active hybrid-emulation sequence is `H0`-`H5` in `ROADMAP.md`.

## Hybrid H0-H5 checkpoint — complete

The hybrid-emulation migration is complete. Native code owns input, presentation,
UI and FMV; the guest owns gameplay, animation, mission state and game audio.
All 20 retail mission resources and 13 distinct overlays pass package loading,
guest bootstrap, immutable presentation, PCM, checkpoint/restore and exact-replay
gates. The production title host advances success through native EOL/SOL movies
to the next mission and restores the guest checkpoint after failure.

Gate: strict headless/PsyCross Release builds, CTest and the H2-H5 supported-ROM
gates are green. See `H3_FINDINGS.md`, `H4_FINDINGS.md` and `H5_FINDINGS.md`.

Each stage is implemented as far as its dependencies allow, verified automatically, and then handed to the user for the stated test. Work on the next stage starts after that test result.

## Stage 1: foundation and executable analysis — complete

- Reproducible MSVC/vcpkg/PsyCross build.
- Strict NTSC-U 1.1 disc identification.
- CUE/BIN, ISO9660 and PS-X EXE ingestion.
- Overlay catalog and deterministic function maps.
- Pinned Ghidra/PSX-loader workflow; main EXE and `TITLE.OVL` imported.
- First native `system.c` recovery: boot order and state stack.

Test gate: PsyCross window/input platform smoke test. No gameplay is expected.

## Stage 2: title and menu — complete

- Recovered and named all 24 discovered `TITLE.OVL` functions.
- Recovered the title menu/operation state and original sprite placement.
- Added validated HOG/TIM readers and direct disc-backed title assets.
- Added raw Mode 2 sector reads and native MDEC/STR plus XA startup playback.
- Added native search/menu update, PsyCross render and pad/keyboard input flow.
- Composed the search/menu sprites over the looping original `TITLE.STR` background.

Test gate: original startup movies/audio, original menu assets, selection navigation,
command dispatch and exit path.

## Stage 3: first mission native vertical slice — superseded

- Recovered the original `INIT.OVL` mission-mount contract.
- Added strict FOG ingestion and mounted the `SUBWAY` package for Georgia Street.
- Validated its texture/model HOGs and connected New Game to `SOL/SUBWAY.STR`.
- Added room visibility and texture streaming from `SUBWAY.DAT`, VLF and both VRAM banks.
- Added active-room terrain rendering with PGXP perspective correction, Z-buffering,
  material-aware back-face rejection, near-plane polygon clipping and
  collision-only polygon filtering.
- Added `SUBWAY.BIN` room-object lists, static DLF GMD/EMD props and native
  class-0x30 two-particle EXPL fire emitters from `COMMON/SPFX.HOG`; fire uses
  authored origins, a depth-tested additive pass and whole-quad near clipping.
- Restored the native four-frame TP10 texture animation for opaque police-car
  `LIGHT.GMD` bars; both lightbars and fire are occluded by walls through PGXP-Z.
- Added validated HMD hierarchy/material decoding and the original PCHAN
  locomotion, stance, aim, weapon-action, roll and death channels; decoded root
  records drive frame-synchronous movement.
- Added a softly damped behind-Gabe chase camera with stateful EMD obstruction,
  first-person manual aim, digital/proportional analog movement, running,
  grounding, provisional wall collision and room transitions.
- Added freely resizable window mode, fast F11/Alt+Enter borderless-fullscreen
  switching and exact restoration of the previous window state.
- Added the original first-mission HUD assets, corrected multi-part weapon icons
  and inventory cycling, plus a platform-neutral in-game menu with briefing,
  options, weapons, objectives, parameters and original three-layer map.
- Added first-person mouse aim, firing/ammo/reload/damage, automatic target lock,
  quick/menu weapon equip and the retail controller action layout.
- Remaining: enemy AI, interactive objects, mission triggers/objectives/checkpoints,
  weapon-specific effects/audio and deterministic frame regressions.

Current test gate: start New Game, confirm the opening movie, enter Georgia Street,
move through the initial rooms, and verify terrain, wall layers, fire occlusion and
near-plane integrity, actor/camera heading, animation, HUD, in-game menu, props and
fullscreen toggles.
The final Stage 3 gate remains aiming, shooting and reaching the first stable
checkpoint after the remaining gameplay paths are recovered.

## Stage 4: full content

- Recover the remaining mission overlays and special cases.
- Remaining XA/STR uses, save/load and controller completion.
- Complete all campaign state transitions.

Test gate: user campaign playthrough and save/load checks.

## Stage 5: hardening

- Fix issues from the playthrough.
- Performance, widescreen/presentation options and packaging.
- Static analysis and final regression suite.

Test gate: release-candidate playthrough.
