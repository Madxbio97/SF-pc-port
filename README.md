# Syphon Filter PC

An unofficial Windows PC runtime for the NTSC-U 1.1 release of *Syphon Filter*
(`SCUS-94240`). The project combines the original PlayStation gameplay code with
a native Windows host for rendering, input, menus, audio and FMV playback.

> This repository does not contain the game or a disc image. A legally obtained
> BIN/CUE image of the supported release is required. The project is not
> affiliated with or endorsed by Sony Interactive Entertainment.

## Public test

The current build is
[Public Test 0.1.0-PT8](https://github.com/Madxbio97/SF-pc-port/releases/tag/v0.1.0-public-test.8).
Download the Windows x64 ZIP from the release page and verify it against the
published `.sha256` file before extracting it.

PT8 fixes streamed VRAM ownership and texture-bank selection so scenery and
actor models survive room transitions, and adds a diagnostic Surface Picker for
future rendering reports. It also refactors the independent 120 Hz SPU/CD/DMA
clock, adds dynamic scene lighting and geometry-based character shadows, restores
original scope labels, and resolves PC-bound button prompts in both languages.

See [CHANGELOG.md](CHANGELOG.md) for the complete release summary.

## Quick start

1. Download `SyphonFilterPC-0.1.0-public-test.8-win64.zip` from the release page.
2. Verify its SHA-256 checksum:

   ```powershell
   Get-FileHash .\SyphonFilterPC-0.1.0-public-test.8-win64.zip -Algorithm SHA256
   ```

3. Extract the ZIP into a new folder.
4. Run `syphon_filter.exe`.
5. Select **BROWSE**, choose the CUE file for *Syphon Filter* USA v1.1, configure
   graphics, controls and **Text language**, then select **DEPLOY**.

Keep every BIN file next to its CUE file and do not rename the files referenced
by the CUE sheet. The launcher remembers the selected image but never copies it
into the game directory.

## Supported game image

Only this revision is supported:

| Field | Required value |
| --- | --- |
| Region | USA / NTSC-U |
| Serial | SCUS-94240 |
| Revision | v1.1 |
| Format | BIN/CUE |
| PS-X EXE SHA-256 | `bac292061ad5bc718ce137ef5b43d3d7e9b1b65248fb0d52229f328ccfe4ab4e` |

The runtime validates the executable from the selected image. Other regions,
revisions, repacks and modified images are not expected to work.

## Runtime features

- Original R3000A mission/gameplay execution with native Windows presentation.
- Complete 20-mission campaign flow, checkpoint restore and persistent mission
  unlock progress.
- Integrated English launcher with game-image selection, arbitrary internal
  resolution, aspect mode, fullscreen, MSAA, bilinear and anisotropic filtering.
- Optional Russian ViT Co. text pack for all 20 missions, menus, briefings,
  weapon descriptions and baked map labels, with a unified Industry Bold 2x
  pixel atlas matching the original Industria-style interface for Latin and
  Cyrillic text.
  Speech, music and FMV remain sourced from the selected USA v1.1 image and are
  never replaced.
- Remappable keyboard/mouse controls and retail-style gamepad controls.
- Native OpenAL presentation of game audio and FFmpeg-backed PS1 STR playback.
- High-resolution, perspective-correct and Z-buffered scene rendering with PGXP
  geometry support.
- Dynamic scene lighting layered over the authored PS1 vertex colours: intact
  mission lamps, fire, alternating police lights, muzzle flashes, explosions
  and the retail flashlight illuminate terrain, props and actors with bounded
  falloff and surface-aware response. Gabe, allies and enemies cast posed,
  light-directed shadows onto authored floors, slopes and moving lifts.
- Restored PS1-style HUD, inventory, objectives, parameters, weapons and map
  pages, including current-position and active-objective indicators.
- Correct scene occlusion for pickups, grenade sprites and transient effects.
- Weapon muzzle flashes for player and enemy weapons, with first-person behavior
  matching the current camera mode.
- Restored glass/window destruction shards and restart-safe destructible state.
- Launcher DOSSIERS gallery with four sharpened bonus pages.

The implementation is still a public test. Gameplay remains driven by the guest
runtime; rendering, platform integration and selected presentation systems are
native. Bug reports should include the mission number, reproduction steps, GPU,
screenshot and the generated log, but never a BIN/CUE image.

## User data and original cheats

Saves and launcher settings are stored outside the installation directory:

```text
%LOCALAPPDATA%\SyphonFilterPC
```

The release archive contains no saves, settings or mission unlocks. Mission
selection normally follows campaign progress. The original PS1 button cheats are
available in their original title/pause-menu contexts and their current state is
shown under **Options > Cheats**. Creating an empty file
named `syphon_filter_cheats` beside `syphon_filter.exe` enables every persistent
cheat and unlocks the complete in-game mission list automatically. The launcher
itself intentionally contains no mission or cheat controls; the marker is
excluded from release artifacts.

## Controls

The launcher exposes all native actions and saves remapped bindings in
`%LOCALAPPDATA%\SyphonFilterPC\launcher.ini`. Default keyboard/mouse and controller
bindings are documented in [docs/CONTROLS.md](docs/CONTROLS.md).

Useful window controls:

- `F11` or `Alt+Enter`: toggle borderless fullscreen.
- `Escape` or `Enter`: open the in-game menu.
- `C`, `X` or Enter: confirm in menus.
- `Z`, `V` or Space: back/cancel in menus.
- `C`, `X`, Enter, `V`, `Z` or Space: skip an FMV.

## Build from source

PsyCross is vendored under `external/PsyCross` because this port contains
renderer, PGXP, filtering, framebuffer and platform changes that are required by
the executable. A separate submodule checkout is not needed.

Prerequisites:

- Windows 10/11 x64.
- CMake 3.24 or newer.
- Visual Studio with the Desktop development with C++ workload.
- vcpkg with `VCPKG_ROOT` pointing to its installation directory.
- Git for cloning the repository.

Configure and build the playable release target:

```powershell
git clone https://github.com/Madxbio97/SF-pc-port.git
Set-Location SF-pc-port
$env:VCPKG_ROOT = 'D:/path/to/vcpkg'
cmake --preset windows-psycross
cmake --build --preset windows-psycross-app-release
```

The executable is written to
`build/windows-psycross/Release/syphon_filter.exe`. No original game data is
needed to compile, but a supported BIN/CUE image is needed to run the campaign.

For the core libraries and complete validation suite:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-release
ctest --preset windows-release

cmake --build --preset windows-psycross-release
ctest --preset windows-psycross-release
```

The Windows preset currently targets the Visual Studio generator recorded in
[CMakePresets.json](CMakePresets.json). See [docs/BUILDING.md](docs/BUILDING.md)
for toolchain overrides, ROM-gated probes and release packaging.

## Command-line launch

The launcher is the normal entry point. These equivalent forms skip image
selection only when a CUE is supplied:

```powershell
.\build\windows-psycross\Release\syphon_filter.exe --game 'D:/Games/Syphon Filter (USA) (v1.1).cue'
.\build\windows-psycross\Release\syphon_filter.exe 'D:/Games/Syphon Filter (USA) (v1.1).cue'
```

Selected options can also be specified directly:

```text
--no-launcher
--resolution=WIDTHxHEIGHT
--fullscreen
--msaa=0|2|4|8
--bilinear | --nearest
--anisotropic | --no-anisotropic
--aspect-adaptive | --aspect-4-3
--vsync | --no-vsync
--fps-limit=0|20..1000
--language=en | --language=ru
```

Development modes:

- `--platform-test <game.cue>`: empty platform smoke test.
- `--title-test <game.cue>`: connected title/campaign compatibility alias.
- `--scene-test <game.cue> --mission=N --no-launcher`: direct mission start,
  where `N` is `1..20`.

## Repository layout

| Path | Purpose |
| --- | --- |
| `apps/` | Executables, launcher and diagnostic probes |
| `assets/` | Port-owned launcher and DOSSIERS presentation assets |
| `external/PsyCross/` | Vendored and modified native PS1 compatibility backend |
| `include/sf/`, `src/` | Runtime, emulation, game and platform implementation |
| `tests/` | Deterministic unit/integration tests |
| `tools/` | Release packaging, generated assets and reverse-engineering helpers |
| `docs/` | Architecture, build, controls, milestones and release documentation |

## Documentation

- [Build and validation workflow](docs/BUILDING.md)
- [User guide](docs/USER_GUIDE.md)
- [Controls](docs/CONTROLS.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md)
- [Release process](docs/RELEASING.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Current stages](docs/STAGES.md)
- [Roadmap](docs/ROADMAP.md)
- [Third-party components](THIRD_PARTY.md)
- [Contributing](CONTRIBUTING.md)

## License

Project-owned source code is available under the [MIT License](LICENSE). Vendored
and dynamically linked dependencies retain their own licenses; see
[THIRD_PARTY.md](THIRD_PARTY.md) and the corresponding files in each component.
No rights to *Syphon Filter*, its game data, characters, artwork or trademarks
are granted by this repository.
