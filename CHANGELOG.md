# Changelog

All notable public-test changes are documented here. The project currently uses
pre-release tags rather than a stable semantic-versioning promise.

## 0.1.0-public-test.11 - 2026-07-28

### Rendering

- Removed the experimental screen-space contact-shadow/SSAO pass completely,
  including its shaders, intermediate framebuffers, launcher option and command
  line switches.
- Retained the PT10 renderer hot-path optimizations and the existing
  geometry-driven dynamic character shadows.

### Release artifact

- File: `SyphonFilterPC-0.1.0-public-test.11-win64.zip`
- Checksum: supplied in the accompanying `.zip.sha256` file.
- Supported disc: *Syphon Filter* USA v1.1 (`SCUS-94240`), BIN/CUE.

## 0.1.0-public-test.10 - 2026-07-28

### Rendering

- Added optional half-resolution SSAO with reversed-Z reconstruction and a
  depth-aware bilateral blur. The effect is composited before transparent
  effects and HUD rendering, so interface art remains clean.
- Removed synchronous OpenGL state queries from the frame, resolve and SSAO
  paths by making renderer-owned state explicit and cached.
- Replaced full VRAM texture refreshes with coalesced dirty-row uploads and
  converted native framebuffer readback directly into guest VRAM.
- Reused dynamic-light, shadow, HUD, callout and weapon-effect scratch storage
  across presentation frames, and moved expensive primitive-capacity planning
  to the original 20 Hz guest tick.
- Reduced SSAO sampling and blur bandwidth while retaining depth-aware edges.

### Release artifact

- File: `SyphonFilterPC-0.1.0-public-test.10-win64.zip`
- Checksum: supplied in the accompanying `.zip.sha256` file.
- Supported disc: *Syphon Filter* USA v1.1 (`SCUS-94240`), BIN/CUE.

## 0.1.0-public-test.9 - 2026-07-28

### Gameplay and input

- Separated first-person WASD locomotion from mouse/right-stick sight input;
  stale guest PAD axes and chase-mode target anchors can no longer kick the
  first-person reticle.
- Smoothed high-refresh first-person camera presentation, lateral movement and
  weapon-switch animation while retaining the original 20 Hz game simulation.
- Restored consistent typewriter animation, wrapping and backdrop bounds for
  gameplay notifications in both English and Russian.

### Rendering

- Stabilized geometry-driven character shadows across ledges, walls and scenes
  with multiple light sources while reducing redundant projection work.
- Corrected depth classification and opaque/transparent submission ordering for
  glass and other translucent world polygons without applying world depth to
  screen-space UI.
- Removed the Surface Picker and its diagnostic payload from public builds.

### Localization and presentation

- Kept original English scope captions for sniper and night-vision weapons and
  hardened partial typewriter packets against accidental Cyrillic remapping.
- Extended regression coverage for first-person input, reticles, message reveal,
  transparent depth and shadow stability.

### Release artifact

- File: `SyphonFilterPC-0.1.0-public-test.9-win64.zip`
- Checksum: supplied in the accompanying `.zip.sha256` file.
- Supported disc: *Syphon Filter* USA v1.1 (`SCUS-94240`), BIN/CUE.

## 0.1.0-public-test.8 - 2026-07-27

### Gameplay and presentation

- Restored the documented USA/PAL retail cheat chords, centralized their
  persistent runtime state and added an original-style **Options > Cheats**
  page; launcher-side mission and cheat controls were removed.
- Corrected the Russian `Ш/Щ/ш/щ` middle stems in the generated Industria atlas
  and retained canonical English labels in sniper/night-vision scopes.
- Reworked Russian map-objective wrapping and replaced frame-rate-dependent
  flashing with a synchronized low-contrast objective glow.
- Protected the final campaign stream from an inherited input edge so the
  credits and post-credits sequence play to completion.
- Added dynamic scene lights and geometry-driven shadows for Gabe, allies and
  enemies, including stable floor/wall projection and bounded translucency.
- Restored original English sniper/night-vision scope labels and resolved every
  button token against the active keyboard, mouse or gamepad binding.
- Isolated mission-menu overrides to the selected locale so Russian objective
  records can never replace the original English guest strings.

### Rendering and streaming

- Reworked VRAM texture aliases around explicit scene generations and complete
  page/CLUT identity, preventing stale room textures and disappearing actors.
- Corrected mission texture-bank validation and retail SCRIM copy semantics
  across streamed segments without weakening invalid-state guards.
- Added an opt-in Surface Picker that highlights one submitted surface and
  writes a complete diagnostic dump on request.

### Audio and timing

- Separated the 120 Hz retail SPU clock from presentation rates up to 240 FPS,
  and made synchronous CD/DMA completion advance devices without generating
  future audio.
- Rebuilt OpenAL buffering and underrun recovery around one bounded monotonic
  timeline, avoiding stale replay, dropped samples and accumulated latency.

### Architecture

- Moved retail cheat definitions, chord detection and state into a dedicated
  game module shared by the title screen, pause menu and gameplay runtime.
- Kept cheat state alive across mission transitions and returns to the title
  screen, with one activation path for both original button codes and menu
  switches.
- Split dynamic lighting, scope-text policy, mission texture ownership and
  Surface Picker diagnostics into testable modules with regression coverage.
- Updated headless gameplay probes to advance the same independently driven
  120 Hz hardware/audio clock as the real frontend, without double-clocking the
  production runtime.

### Release artifact

- File: `SyphonFilterPC-0.1.0-public-test.8-win64.zip`
- Checksum: supplied in the accompanying `.zip.sha256` file.
- Supported disc: *Syphon Filter* USA v1.1 (`SCUS-94240`), BIN/CUE.

## 0.1.0-public-test.7 - 2026-07-25

### Russian localization

- Added the complete text-only Russian language pack for all 20 missions,
  including proofread menus, objectives, parameters, briefings, gameplay
  messages, weapon descriptions and baked map/title labels.
- Rebuilt the ViT-compatible font sheets as a unified 2x Industria-style atlas
  while preserving the retail byte map, advances and logical menu geometry.
- Fixed incomplete or mismatched mission briefings, missing weapon descriptions,
  weapon specification tables and every observed spelling/case variant of the
  gas-grenade pickup message.
- Kept speech, music and FMV on the original USA v1.1 disc; the pack changes
  presentation text only.

### Presentation and runtime

- Finished the original two-page Weapons presentation and restored complete
  descriptions, ammunition data and the four authored specifications.
- Corrected text flow, pagination and placement across briefing, objective,
  parameter, map, weapon and options pages for both supported languages.
- Added working vertical synchronization and a high-resolution frame limiter,
  and removed the guest CPU-overclock workaround from normal gameplay timing.
- Replaced restart-prone gameplay audio queues with a bounded continuous
  callback stream, stock-rate SPU scheduling and deterministic transition reset.
- Corrected scene ordering and depth behavior for translucent polygons, pickups,
  grenade sprites and transient effects without bypassing authored occlusion.
- Preserved mission completion/save flow, campaign unlock progress and
  restart-safe destructible state.

### Launcher and architecture

- Corrected launcher layout and Unicode language labels, and exposed the new
  Russian text pack through the persistent language selector.
- Split file I/O, localization, pause-menu data, retail map projection, VRAM,
  runtime guards and native font upload into explicit modules with automated
  architecture checks.
- Updated the public packager to require and include the complete `locales`
  directory while continuing to reject disc images, saves, logs, cheats and
  developer binaries.

### Release artifact

- File: `SyphonFilterPC-0.1.0-public-test.7-win64.zip`
- Checksum: supplied in the accompanying `.zip.sha256` file.
- Supported disc: *Syphon Filter* USA v1.1 (`SCUS-94240`), BIN/CUE.

## 0.1.0-public-test.6 - 2026-07-24

### Stability

- Fixed a renderer/UI bridge fault in the PHARCOM warehouse missions when the
  retail streamer recycled an active world's relocated vertex-color payload.
- Kept guest world visibility and geometry authoritative while treating the
  transient per-vertex lighting payload as an optional presentation cache.
- Extended the headless retail environment probe and validated all three
  warehouse missions past the reported failure frame.

### Release artifact

- File: `SyphonFilterPC-0.1.0-public-test.6-win64.zip`
- Checksum: supplied in the accompanying `.zip.sha256` file.
- Supported disc: *Syphon Filter* USA v1.1 (`SCUS-94240`), BIN/CUE.

## 0.1.0-public-test.5 - 2026-07-24

### Launcher and distribution

- Replaced the command-script bootstrap with an integrated Windows launcher.
- Added first-run CUE selection, persistent graphics/input settings and styled
  auxiliary dialogs.
- Added an English **DOSSIERS** button and a four-page bonus gallery.
- Added a new multi-resolution launcher icon featuring Gabe Logan.
- Added a controlled release packager with clean-install checks, dependency
  licenses, per-file SHA-256 sums and an archive checksum.
- Ensured public packages contain no save data, settings, game images or
  `syphon_filter_cheats` marker.

### Rendering and graphics

- Applied the selected resolution to the internal color/depth render targets,
  not only the UI.
- Added independent anisotropic filtering and seam-safe bilinear texture
  filtering that clamps PS1 atlas tiles without bleeding across their edges.
- Added selectable MSAA and original/adaptive aspect modes.
- Improved FMV presentation and removed the additional dithering pass.
- Reworked weapon muzzle flashes with a smaller textured star shape and reliable
  player/enemy shot triggering.
- Corrected first-person muzzle-flash rules: the player's own flash is hidden
  while enemy flashes remain visible.
- Restored depth occlusion for pickups, grenade sprites, blood, sparks and other
  transient effects.
- Fixed scene lighting on weapon crates and multiple level texture/model mapping
  errors, including the Kazakhstan gas tank case.
- Removed the development FPS counter from the game image.

### Gameplay presentation

- Restored every documented retail cheat and its original title/pause-menu
  button context, including PAL aliases, infinite ammunition, hard mode,
  one-shot kills, weak enemies, stage select and the Georgia Street theater.
- Added the original-style **Options > Cheats** page with synchronized switches
  for all six restored modes.
- Removed mission selection and cheat controls from the launcher permanently;
  `syphon_filter_cheats` now activates persistent retail cheats directly.

- Rebuilt the pause map presentation around the original PS1 layout, including
  map layers, current Gabe position and active-objective indicators.
- Reworked Objectives, Parameters, Options and Weapons pages, including the
  three weapon-stat bars and full-information panel backgrounds.
- Muted world audio while the pause menu is open while retaining menu sounds;
  restored audio levels on close.
- Fixed campaign mission unlock progression and preserved the highest unlocked
  mission when replaying an earlier stage.
- Gated unrestricted mission selection behind an explicit local
  `syphon_filter_cheats` marker.
- Restored the grenade sprite, ballistic flight path and scene occlusion for
  player and enemy throws.
- Fixed disappearing held weapons and bomb/destructible models during missions.
- Restored window, glass-panel and stained-glass destruction into visible shards.
- Fixed destructible state restoration after both manual restart and
  failure-triggered mission restart.

### Release artifact

- File: `SyphonFilterPC-0.1.0-public-test.5-win64.zip`
- SHA-256: `4B99A0EE167C0F9C649E36010D70ADD2480682A245265E9C32CB3D098063F403`
- Supported disc: *Syphon Filter* USA v1.1 (`SCUS-94240`), BIN/CUE.
