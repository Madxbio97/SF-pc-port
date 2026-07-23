# Changelog

All notable public-test changes are documented here. The project currently uses
pre-release tags rather than a stable semantic-versioning promise.

## Unreleased

- No changes after Public Test 0.1.0-PT5.

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
