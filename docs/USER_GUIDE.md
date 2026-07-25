# User guide

## Installation

1. Download the latest Windows x64 ZIP from the GitHub Releases page.
2. Compare its SHA-256 hash with the `.zip.sha256` sidecar.
3. Extract the complete archive into a new writable folder.
4. Run `syphon_filter.exe`; no CMD bootstrap is required.

Do not run the executable from inside the ZIP. The DLLs, dossier pages and license
files must remain beside it in their packaged layout.

## Selecting the game image

The port needs a legal BIN/CUE image of *Syphon Filter* USA v1.1 (`SCUS-94240`).
Select the CUE file with **BROWSE**. Keep its referenced BIN files in the same
relative locations. The launcher remembers only the path.

No original image is included in the release or source repository. Other regions
and revisions are rejected because their executable and overlays differ.

The bundled Russian option is a text-only language pack extracted from the ViT
Co. localization. Continue selecting the supported USA v1.1 CUE for gameplay;
the launcher applies Russian fonts and text on top of it. Voices, music and FMV
remain unchanged.

## Launcher options

- **Resolution** controls the internal scene and depth buffers as well as output.
- **Aspect** chooses original 4:3 framing or adaptive Hor+/Vert+ framing.
- **Fullscreen** starts in borderless desktop fullscreen.
- **MSAA** selects disabled, 2x, 4x or 8x multisampling.
- **Bilinear filtering** smooths textures while clamping each PS1 atlas tile to
  avoid seams and neighboring-texture bleed.
- **Anisotropic filtering** independently improves oblique world textures.
- **Vertical synchronization** presents on the display refresh and removes
  tearing.
- **Frame limit** applies a high-resolution cap to every presented frame. Use
  `Unlimited` when VSYNC or variable-refresh hardware should own the cadence.
- **Text language** selects English or the Russian ViT Co. text pack. The choice
  is remembered in `%LOCALAPPDATA%\SyphonFilterPC\launcher.ini`.
- **Controls** remaps all keyboard and mouse gameplay actions.
- **DOSSIERS** opens the four-page bonus gallery; use the on-screen buttons,
  Left/Right, A/D or Escape.

Select **DEPLOY** to save the settings and start the game.

## Saves and mission selection

User data is stored in:

```text
%LOCALAPPDATA%\SyphonFilterPC
```

Campaign progress remembers the highest unlocked mission. Replaying an earlier
mission does not erase later unlocks. In a clean installation only legitimately
unlocked missions are selectable.

For development only, an empty `syphon_filter_cheats` file beside the executable
unlocks the complete mission list. Public releases and this repository exclude
that file.

## Pause menu

Open the in-game menu with Escape or Enter. The active page reproduces the PS1
map/objective/parameter/briefing/weapon/options structure. Opening the menu mutes
world audio but menu sounds remain active; closing it restores the previous mix.

Map pages show the current position on the correct layer and active objectives
with highlighted indicators. Weapon details include description, ammunition,
rate/damage information and the original three stat bars.

## Clean reset

To test a completely clean user profile, close the game and move the
`%LOCALAPPDATA%\SyphonFilterPC` directory to a backup location. Deleting it
permanently removes local saves and launcher settings; the release ZIP itself
never contains them.

## Reporting a problem

Provide the public-test version, mission, checkpoint/area, reproduction steps,
graphics settings, GPU/driver and a screenshot or video. Attach the generated log
when useful. Never upload your BIN/CUE image.
