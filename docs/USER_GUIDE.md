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
- **Antialiasing** selects Off, depth-aware SMAA Ultra, or hardware MSAA
  2x/4x/8x. SMAA and MSAA are alternatives and never run together.
- **Bilinear filtering** smooths textures while clamping each PS1 atlas tile to
  avoid seams and neighboring-texture bleed.
- **Mipmapped trilinear** reduces distant texture shimmer with a palette-safe
  per-tile mip reconstruction.
- **Anisotropic filtering** independently improves oblique world textures and
  uses the trilinear mip reconstruction when both options are enabled.
- **SMAA Ultra** combines colour and depth edges on the 3D scene before the HUD
  is drawn, improving silhouettes while keeping text and optics sharp.
- **Vertical synchronization** presents on the display refresh and therefore
  caps FPS to the monitor's current refresh rate.
- **Frame limit** applies a high-resolution cap to every presented frame. Use
  `Unlimited` when VSYNC or variable-refresh hardware should own the cadence.
- **Text language** selects English or **Русский**. The choice
  is remembered in `%LOCALAPPDATA%\SyphonFilterPC\launcher.ini`.
- **Controls** remaps all keyboard and mouse gameplay actions.
- **DOSSIERS** opens the four-page bonus gallery; use the on-screen buttons,
  Left/Right, A/D or Escape.

Select **DEPLOY** to save the settings and start the game.

Press **F6** during gameplay to show or hide presentation FPS and frame time.
For a 240 FPS check, disable vertical synchronization, select 240 FPS or
Unlimited, and follow [PERFORMANCE.md](PERFORMANCE.md).

## Campaign difficulty

Starting **New Game** opens a three-item difficulty selector:

- **Normal / Оригинал** preserves the original retail balance.
- **Hard Mode / Высокая сложность** enables the retail hard-mode enemy aim and
  reaction rules. It is a campaign setting and is no longer listed as a cheat.
- **Agent / Агент** includes Hard Mode behavior, increases damage received by
  the player by 25%, improves enemy accuracy and lets enemies track movement
  more effectively at medium range. On each new hostile SVD/sniper targeting
  engagement, a localized Head Shot leader appears above Gabe. After a
  one-second grace period, the same enemy's next ballistic hit is a guaranteed
  one-shot kill; the warning remains visible while that threat is active.

Agent starts with its own localized difficulty notice instead of the Hard Mode
notice.

Selecting Agent first opens a localized warning that identifies it as a
PC-version addition and summarizes its higher enemy threat and stricter mission
conditions. Back returns to the difficulty selector without starting a game.

### Mission-specific Agent changes

The current build enables only these validated mission-specific overrides:

- **Georgia Street:** Kravitch carries an ITHACA-37 shotgun, fires more often
  and repositions through the original route/LOS controller after shooting.
- **Main subway line:** the HUD shows Aramov's escape progress, and her
  horizontal movement is 25% faster.
- **Washington Park:** the timer is 15:00; Marcos uses ordinary fragmentation
  grenades at a faster cadence; each damaging player hit on a validated CBDC
  bomb technician deducts 30 seconds.
- **Freedom Memorial:** the HUD shows bomb-detonation and Girdeux fuel-tank
  damage meters. The bomb budget is capped at 100%: shotgun hits add 50%, .45
  hits 40%, M-16 hits 10%, and 9 mm-class weapons and rifles add 2%. An M-79
  hit or a thrown fragmentation/gas grenade fills it immediately; the taser
  and flashlight add nothing. Reaching 100% invokes the retail bomb failure.
- **Expo Center Dinorama:** the HUD shows Phagan and Aramov health meters.
- **Rhoemer's Base:** Gabrek carries an M-16 and fragmentation grenades.
- **Base Escape:** the escape timer is 2:24.
- **Stronghold lower level:** the three validated guards around the chapel and
  catacomb entrance carry shotguns.
- **PHARCOM elite guards:** active authored fragmentation-grenade users use a
  faster grenade cadence; the rule does not apply to gas-grenade users.
- **Warehouse 76:** the collapse timer and its mission parameter are 12
  minutes.
- **Tunnel blackout:** while Gabe's flashlight is active, Agent enemy target
  memory is 100 game frames instead of the normal Agent value of 80.

The selected difficulty is stored in every occupied save slot and is shown next
to the mission name in save/load lists. Existing V1-V4 saves are migrated as
Normal without losing campaign progress.

## Saves, mission selection and retail cheats

User data is stored in:

```text
%LOCALAPPDATA%\SyphonFilterPC
```

Campaign progress remembers the highest unlocked mission. Replaying an earlier
mission does not erase later unlocks. In a clean installation only legitimately
unlocked missions are selectable.

The launcher contains no mission selector or cheat controls. In-game cheat state
is visible and can be switched under **Pause > Options > Cheats**. The original
PS1 codes also work in their retail contexts:

- **All weapons / infinite ammo:** pause, highlight **Weapons**, hold
  Right + L2 + R2 + Square + Circle + X.
- **One-shot kills:** pause, open **Weapons**, highlight **Silenced 9mm**, hold
  Left + Select + Square + X + L1 + R2.
- **Stage select:** pause, open **Options**, highlight **Select Mission**, hold
  Left + L1 + R1 + Select + Square + X.
- **Weak enemies:** pause, highlight **Map**, hold Right + L1 + R1 + X.
- **Movie theater:** at the Georgia Street theater door, pause, highlight
  **Map**, hold L2 + R1 + X + Right.

The documented PAL aliases are accepted as well. For development, an empty
`syphon_filter_cheats` file beside the executable enables all persistent modes
and the complete in-game mission list without exposing any launcher controls.
Public releases and this repository exclude that file.

## Pause menu

Open the in-game menu with Escape or Enter. The active page reproduces the PS1
map/objective/parameter/briefing/weapon/options structure. Opening the menu mutes
world audio but menu sounds remain active; closing it restores the previous mix.
The Options page includes a Cheats screen for all five restored retail modes.

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
