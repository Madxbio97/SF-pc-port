# Weapon Box and gameplay-menu follow-up — 2026-11-18

Reference: the user's USA v1.1 image (SCUS-94240), authored BIN/GMD/TIM data and
original callbacks executed through the local R3000 VM. Interactive gameplay
testing remains with the user.

## Completed

- ~~Keep resident Weapon Boxes visible independently of transient DAT membership.~~
- ~~Cover both WEPCRATE/WEPCRATX and WEPCRABE/WEPCRABX model pairs.~~
- ~~Render SUBWAY2's TNT/C4 cache instead of treating it as an invisible anchor.~~
- ~~Load all nine original WEP/TNT atlas rectangles and their shared palette.~~
- ~~Select the TNT open model from the original consumed latch.~~
- ~~Restore the original menu-opening sound.~~

## Models and textures

The missing boxes had three related presentation problems:

1. Room membership could hide a live resident pickup while its original
   interaction/callout remained available.
2. The crate texture classifier recognized WEPCRATE/WEPCRATX but omitted the
   small WEPCRABE/WEPCRABX pair used by armour pickups.
3. SUBWAY2 source/slot 279, definition 20, class 0x57 had an explicit native
   skip for TNTCRATE/TNTCRATX. Its TNT texture rectangles were also missing.

The native renderer now retains the exact authored crate geometry for the
guest-owned resident lifetime. Explicit dormant/hide and retirement still win.
The consumed latch is instance byte +0 bit 5, as in original FUN_8008cb5c and
FUN_80090624. Open models are selected without changing health, collision,
inventory grants or objective logic. Checkpoint restore returns the matching
original/native presentation together.

The original interface atlas uses logical pages 12/13 and CLUT 0x78f0. All nine
TIM rectangles share the same 256-colour palette:

- WEPTRIM, WEPINNER, WEPTOP
- TNTINNER, TNTSIDE, TNTTOPA, TNTTOPB, TNTTRIMA, TNTTRIMB

The first new regression correctly rejected TNTCRATE at page 13, UV (16,223):
the old three-image WEP overlay did not cover that texel. Loading the six TNT
rectangles restores coverage using the original positions. Each active bank
uses the existing physical-page/CLUT remapping and generation tracking.

The special C4 identity remains only for its authored callout anchor; it no
longer suppresses geometry, shadows or primitive-budget accounting.

## Menu sound

A local original-input trace, after the opening radio completes, recorded
bank 5 / sound 2 from return site 0x80140084 in MENU.OVL's initial Map callback
0x80140058. This is COMMON/BEEPSX's existing navigation cue, whose PCM is already
checked by the H3 audio gate (navigation PCM hash: 0x6fb98f4448ea13b3).

The PC pause-entry branch set the UI volume and paused gameplay audio but never
played a cue. It now triggers that original sample once on the keyboard or
controller pause edge, through the independent UI source. Mission SPU/XA timing
and configured sound-effects volume retain their existing handling.

The older standalone H3 probe still expected submitted host PCM to survive a
VM reset and relied on implicit audio ticks. It now checks the next explicit
hardware tick after reset, advances the audio clock during gameplay, and
compares two checkpoint restores because retail restarts the music sequence.
The probe is registered as an opt-in ROM CTest to keep those checks current.

## Verification

- Portable Release: 24/24 standard tests passed.
- Original-ROM Release: 23/23 tests passed (21 campaign gates from the full
  run, then the corrected pickup gate and newly registered H3 audio gate).
- PsyCross Release: 28/28 tests passed, including hidden OpenGL validation.
- H3: 884,205 PCM frames across 400 gameplay updates; audible output, exact
  reset continuation and deterministic checkpoint restart passed.
- Both Release builds and git diff --check passed.

The new opt-in sf_retail_pickup_model_probe checks every mission's inventory
crate definitions and detects unknown model families or missing authored
resources. Five authored lockers (LCKRA/LCKRC/GLCKRA families) also retain
both original meshes. BUNKER class-0x50 sources 313..320 have no model in the
original BIN; the probe verifies that explicit exception rather than inventing
geometry for those triggers. It validates 126 resident crates, 252 closed/open model instances,
16,692 drawable triangle UV corners, nine compatible nontransparent texture
rectangles, and these presentation states:

- initial closed/open state and active-scene membership;
- the original consumed latch selecting the open model;
- explicit dormant/hide still removing the model;
- actual checkpoint restoration recovering its model and active membership.

The latch replay is a controlled original-RAM fixture. It does not claim a
complete manual pickup interaction at every location, or identical complete
framebuffer output.

## Manual checks

- Georgia Street: armour boxes appear before collection and use the appropriate
  open model afterward.
- Destroyed subway: the C4 cache under the interaction label has visible geometry
  and textures; collecting C4 still works.
- Stronghold / lower level and later missions: weapon and armour boxes survive
  room changes and checkpoint restarts.
- Open the gameplay menu with keyboard and controller: opening feedback is
  audible once, follows Sound FX volume and does not repeat while holding Pause.

## Reproduction

~~~powershell
cmake --preset windows-msvc -DSF_BUILD_ROM_PROBES=ON '-DSF_SUPPORTED_ROM_CUE=<supported.cue>'
cmake --build --preset windows-release
ctest --preset windows-release --output-on-failure -j 3
# Return to the normal portable build:
cmake --preset windows-msvc -DSF_BUILD_ROM_PROBES=OFF -DSF_SUPPORTED_ROM_CUE=
cmake --build --preset windows-psycross-release
ctest --preset windows-psycross-release --output-on-failure
~~~

No original game data, memory dumps, settings or saves are distributed.
