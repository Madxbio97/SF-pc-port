# Build and validation workflow

This document describes reproducible Windows builds for contributors. Building
the program does not require proprietary game data. Retail-ROM probes are
separate and opt-in.

## Prerequisites

- Windows 10/11 x64.
- CMake 3.24 or newer.
- Visual Studio with MSVC, the Windows SDK and the Desktop development with C++
  workload.
- A current vcpkg checkout.
- Git.

Set `VCPKG_ROOT` before configuring the PsyCross build:

```powershell
$env:VCPKG_ROOT = 'D:/Tools/vcpkg'
```

The manifest in `vcpkg.json` pins its baseline and installs SDL2, OpenAL Soft and
the required FFmpeg libraries. PsyCross is vendored at `external/PsyCross`; do
not initialize a submodule.

The checked-in preset names a Visual Studio generator explicitly. If your Visual
Studio release uses a different generator name, create a local
`CMakeUserPresets.json` that inherits the checked-in preset and overrides only
the generator/toolchain details. That file is ignored by Git.

## Playable build

Configure once and build only the application target during normal development:

```powershell
cmake --preset windows-psycross
cmake --build --preset windows-psycross-app-release
```

Equivalent explicit target build:

```powershell
cmake --build build/windows-psycross --config Release --target syphon_filter
```

Output:

```text
build/windows-psycross/Release/syphon_filter.exe
```

The build also stages the required dossier images and runtime DLLs beside the
executable. Do not launch from an intermediate directory that lacks those files.

## Core-only build

For headless gameplay/emulator work:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-core-release
```

This stops at `sf_game` and avoids compiling the platform executable.

## Full validation

The full presets compile every enabled target and run the deterministic tests:

```powershell
cmake --build --preset windows-release
ctest --preset windows-release

cmake --build --preset windows-psycross-release
ctest --preset windows-psycross-release
```

Use `--output-on-failure` if invoking CTest without the preset:

```powershell
ctest --test-dir build/windows-psycross -C Release --output-on-failure
```

No validation step launches the game window unless an interactive executable is
started manually.

## Supported-ROM probes

ROM-dependent probes are disabled in a clean checkout. Enable them only with a
legally obtained USA v1.1 CUE:

```powershell
cmake --preset windows-psycross `
  -DSF_BUILD_ROM_PROBES=ON `
  -DSF_SUPPORTED_ROM_CUE='D:/Games/Syphon Filter (USA) (v1.1).cue'
cmake --build --preset windows-psycross-release
ctest --preset windows-psycross-release -L rom
```

Disable them again with `-DSF_BUILD_ROM_PROBES=OFF`. Never commit or upload BIN,
CUE, extracted retail data, saves or generated logs.

## Inspecting an image

The `sf_tool` diagnostic executable can validate the image without starting the
game:

```powershell
.\build\windows-msvc\Release\sf_tool.exe inspect 'D:/Games/Syphon Filter (USA) (v1.1).cue'
.\build\windows-msvc\Release\sf_tool.exe inspect-title 'D:/Games/Syphon Filter (USA) (v1.1).cue'
.\build\windows-msvc\Release\sf_tool.exe inspect-mission 'D:/Games/Syphon Filter (USA) (v1.1).cue'
.\build\windows-msvc\Release\sf_tool.exe catalog 'D:/Games/Syphon Filter (USA) (v1.1).cue'
```

The supported PS-X executable SHA-256 is:

```text
bac292061ad5bc718ce137ef5b43d3d7e9b1b65248fb0d52229f328ccfe4ab4e
```

## Regenerating the Russian text pack

The checked-in Russian pack changes text only: authored mission tables and
the map images whose labels are baked into their pixels. Regenerate it from a
legal ViT Co. image while using USA v1.1 as the English key source:

```powershell
.\build\windows-msvc\Release\sf_tool.exe export-vit-language-pack `
  'D:/Games/Syphon Filter ViT/Syphon Filter.cue' `
  'D:/Games/Syphon Filter (USA) (v1.1).cue' `
  '.\assets\locales\ru-vit'
```

The export step restores the retail ViT font sheets. The checked-in atlas is
then regenerated from the user-supplied Industry Bold Cyrillic face. The generator
preserves the one-byte ViT Cyrillic map, retail advances and logical UVs while
building a 2x physical atlas. The native UI renderer samples the 16-pixel
glyphs through the original 8-pixel coordinate system, so existing layout and
backdrop bounds remain stable:

```powershell
python tools/generate_sf_cyrillic_fonts.py `
  --source assets/locales/ru-vit/fonts `
  --output assets/locales/ru-vit/fonts `
  --font tools/fonts/industry/Industry-Bold_RUS.ttf `
  --font-size 15 `
  --font-weight 700 `
  --atlas-scale 2 `
  --metrics docs/images/sf-cyrillic-font-metrics.json `
  --preview docs/images/sf-cyrillic-font-atlas-preview-v6.png `
  --contact docs/images/sf-cyrillic-font-contact-v6.png
```

The resulting nearest-neighbour atlas preview, contact sheet and advance table
are stored under `docs/images/`. `Industry-Bold_RUS.ttf` must be supplied by the
developer and is deliberately ignored by Git; only its copyright notice and the
generated game atlas are kept in the project. Neither command copies audio,
voices, FMV or a game image.

## Packaging a public test

Build `syphon_filter` first, then run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File tools/package_windows_release.ps1 `
  -Version 0.1.0-public-test.8 `
  -Configuration Release
```

The script stages the executable, exact runtime DLLs, dossier assets, license
texts and public-test documentation; rejects missing dependencies; rejects saves,
game images, the cheat marker and developer artifacts; writes per-file hashes;
and produces a ZIP plus a `.zip.sha256` sidecar under `dist/`.

The script refuses to overwrite an existing release. Move an old artifact aside
or choose a new version. See [RELEASING.md](RELEASING.md) for the complete release
checklist.

## Build-directory hygiene

Visual Studio intermediates contain absolute paths. Do not copy or move a
configured `build/windows-*` directory between source-tree locations. If MSBuild
reports `MSB8028` or a toolchain path changed, remove only the affected build
directory and configure that preset again. Source files and the vcpkg checkout
are unaffected.
