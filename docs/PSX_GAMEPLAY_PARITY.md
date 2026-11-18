# Gameplay parity audit — 2026-11-17

Reference: the user's supported USA v1.1 image, SCUS-94240, executed through
the integer R3000/GTE. The original program remains authoritative for gameplay.
This is an audit of reproducible contracts, not a certification of every possible
playthrough. Manual gameplay testing remains with the user.

## Corrections in this pass

- ~~Remove the extra floor-to-head offset from live skeleton-space cover rays.~~
- ~~Keep crouched source and target roots in their actual animated positions.~~
- ~~Check the segment from the body centre to every extra visibility sample,
  including vertical head/lower-body samples, not just shoulders.~~
- ~~Add geometric regressions for a full wall, low cover, a crouched shooter,
  exposure/retraction, thin side walls, a ceiling and coincident roots.~~
- ~~Restore exact original player/enemy grenade positions and their final linked frame.~~
- ~~Keep completed high-pitch SPU voices restorable in checkpoints.~~
- ~~Restore the checkpoint after ordinary player death even without failed objectives.~~
- ~~Run the CATACOMB gas scenario through death, fade and checkpoint restoration.~~
- ~~Extend the PAD grenade gate to frag and gas: original selection, charge,
  release, projectile identity and quick tap after checkpoint restoration.~~
- ~~Add a campaign grenade gate for every mission allowed by the original grant.~~
- ~~Correct the control reference: held crouch, stationary manual aim, corner
  peek and the original scope zoom bindings.~~

## Root-space evidence and cover behaviour

FUN_8003352c copies the attacker's instance->motion root into the shot origin.
FUN_8006075c publishes the AI's DAT_8011e660 anchor from the display root MATRIX.
Those positions are already above the ground. The old host confirmation treated
them as foot positions: it subtracted another 225 from the source and up to 275
from the target. Its rays could therefore pass above a wall covering the body.

A neutral-input MUSEUM2 reference capture measured:

| Sample | Ground Y | Root Y | Head-joint Y | Settled original L1 eye Y |
| --- | ---: | ---: | ---: | ---: |
| Standing | -644 | -751 | -813 | -823 |
| Crouched | -644 | -702 | -763 | -774 |

The helper now starts at the actual source root. Target samples cover the root,
upper body, lower body and shoulders; the 72-unit upper offset is the original
L1 root-to-eye distance. Lower offsets are conservative interior samples, not a
replacement for the original hit mesh. Every displaced sample must also remain
connected to the body centre without crossing geometry. This prevents invented
head/shoulder points from escaping through walls or an overhang.

The same confirmation serves positive visual acquisition, hostile bullets,
PARK2 contact damage and the existing DANGER guard. Original negative results,
range/FOV, hit probability, damage, hearing, search memory, target selection and
scripted alerts remain owned by the original routines. A positive confirmation
never creates a hit or target of its own.

The previous [cover/stealth report](PSX_OCCLUSION_FIXES.md) records the exact
instruction signatures, call sites and unchanged-event boundaries. Girdeux's
flame animation can still run against cover; occluded contact damage is filtered.
Particle clipping and a complete physical flame simulation are separate work.


## Grenade transform and checkpoint corrections

The original display-coordinate MATRIX is already in the same down-positive
world space as native presentation. Its physics controller uses the opposite Y
sign; FUN_80048628 explicitly negates display Y when seeding the motion state.
The native code nevertheless negated display Y again in its fallback and, for
player throws, could replace it with a separately reconstructed parabola. That
disagreed with the original collision/bounce path. For example, a SUBWAY2 sample
had original display Y=1014 and physics Y=-1014, while native presentation used
Y=952.

Player and enemy grenades now retain all three coordinates and rotation from
the original display MATRIX. The campaign callback gate independently follows
descriptor->display->root-MATRIX in live guest RAM and compares every presented
coordinate and rotation component with that reference. It does not require a
free parabola in a room where the original grenade collides immediately.

FUN_80026808 increments the age byte and tests its previous value against 0x3d.
Age 61 is still linked; gas impacts also jump to 61 before the next callback
unlinks the object and emits the cloud. A shared last-age contract now keeps
that last original frame in both the VM reader and immutable-frame validator.
Age 62 is rejected.

The extended gate also found an unrelated checkpoint failure in SUBWAY3. At a
captured point, a completed SPU voice had pitch_counter=8985, above the 12-bit
fractional range. At high pitch, the ADPCM-end branch could stop a voice before
the advance loop consumed its remaining whole-sample steps. The validator then
rejected the VM's own snapshot. Retaining only the fractional phase fixes this
without changing active-voice PCM or relaxing snapshot validation. A real
non-looping ADPCM block at pitches 0x3000 and 0x3fff now tests completion, snapshot
restore and a later key-on.

## Gameplay coverage matrix

| System | Automated reference/contract | Limit of that evidence |
| --- | --- | --- |
| Movement, rooms and terrain triggers | G2 active gameplay across 20 missions; natural PAD route/stream edges | Does not traverse every ledge, slope or room in a full playthrough |
| Actor lifetime and protected actors | G3.1/G3.2/G3.3: exact identities, dormant activation, pose/lifetime and snapshot replay | Remote/dormant actors are checked as dormant until their authored trigger |
| AI and combat | G3.4: authored candidate in each mission, targeting, awareness, damage/death or rigid-boss/inactive contract | Does not substitute a dormant-contract check for a complete boss fight |
| Stealth and hearing separation | CATACOMB PAD-only follow/exposure gate; positive-vision cover gate; unrelated alert callbacks pass through | Five-room topology and first follow edge do not constitute the full tailing mission |
| Weapons and ammunition | 26 recovered weapon records; original equip/menu, damage and inventory code; Agent weapon/controller gates | Utility-item use still needs scene-specific manual comparison |
| Frag/gas grenades | PAD charge/release and quick tap in Georgia Street/BASEEXT/CATACOMB; original throw-callback transforms and checkpoint restore across the allowed-mission grant matrix | Callback replay does not prove uninterrupted input/animation in every combat scene, identical explosion/gas particles or every victim interaction |
| Interactions and objectives | G2 scripts, G3.1 authored events and G3.5 overlay callbacks across the campaign | Callback/event replays are not a complete sequence of natural mission solutions |
| Checkpoints and progression | G4: ordinary deaths, objective failures, successes, carry/inventory, checkpoint restore and EOL stage/finalize across the campaign | Host persistence/transition checks do not prove every possible mid-combat checkpoint state |
| Gameplay presentation | All-mission environment, sprite and lighting regressions; native HUD/input tests | Complete-frame PSX/PC comparison remains a manual scene check |

The opt-in probes use the supplied image locally. They do not control the user's
open game, alter the installed saves or distribute ROM data.

## Grenade reference boundaries

Original FUN_80047c0c deliberately grants frag outside mission indices 5..13;
it grants gas only in 7..13 and 19. The campaign gate therefore exercises
**19 cases across 18 missions**. MUSEUM/MUSEUM2 (indices 5/6) have neither family
in that original grant and are not counted as throw coverage.

PAD fixtures wait for a quiet neutral interval after startup radio. A
control-ready player alone does not mean the viewport/audio input takeover has
ended. The charged and quick-tap cases in Georgia Street, BASEEXT and CATACOMB restart
from the same real checkpoint to isolate the previous projectile/cooldown.

The campaign transform fixture prepares the weapon selector at descriptor+4
exactly as original release handler FUN_80025dfc does, then invokes the original
throw animation callback FUN_80026608 at the captured boundary. The original
scheduled physics runs normally afterward.
There are no host-authored projectile coordinates. This deliberately isolates
transform/age/checkpoint coverage from interruptions during wind-up: PARK2's
start accepts Square input but incoming damage can interrupt the animation
before the projectile callback. That scenario is not labelled a dropped input
or successful all-mission PAD coverage. The callback gate stops at original
projectile unlink, then verifies checkpoint restore; it does not certify the
aftermath of a throw.

## Validation before the death/restart follow-up

- Portable Release: **24/24** standard tests.
- PsyCross Release: **28/28**, including the hidden OpenGL subsystem test.
- All **20** opt-in original-image gates passed across the full run and targeted
  reruns. The full enabled CTest run passed its other 43 tests; the revised
  campaign grenade command then passed all **19 cases / 18 missions** with
  **857 exact MATRIX comparisons** and successful checkpoint restoration.
- PAD grenade gate: **3 cases**, Georgia Street frag and BASEEXT/CATACOMB gas;
  original charge growth, release, short click and checkpoint restoration pass.
- Original occlusion gate: **20 missions**, **54 replays**, **21 positive
  acquisitions**; the corrected geometry rejects **17** blocked samples.
  Both PARK2 flame contact sites reject covered hits. Clear-path return values
  and complete 2 MiB RAM remain unchanged in paired reference replays.
- CPU regression: **100 consecutive passes** after moving three retained VM
  snapshots out of the large fixture's stack. Windows had reported 0xc00000fd
  on intermittent earlier runs; the process stack limit was not increased.
- Sprite regression: **943 replays**, zero mismatches.
- Lighting regression: **57,344 samples**, zero mismatches across 20 missions.

The campaign callback result certifies linked projectile transforms and restore,
not every aftermath or manual gameplay scenario described below.

## Resolved gas aftermath and ordinary death

The previous CATACOMB callback scenario exposed a production restart defect.
The original gas killed Gabe at native update 8 and completed its death/fade
path at update 90. Terminal and failed-objective flags remained clear.
The host checked only mission.failure before deciding whether to run state 2's
original reinitialization. It entered that loader and exhausted its instruction
budget at 0x80028930 while rebuilding animation tables.

Original FUN_80017630 tests the player's health directly and calls FUN_80017778
on death. FUN_80017778 enters FUN_80016184(3), which posts state 2 and transition
3. This is independent of the objective-failure bytes. The host now recognizes
a valid dead player at that same state-2 boundary and uses its existing coherent
checkpoint consumer. It does not stop the original death animation/fade early
or set invented terminal/failure flags. Successful terminal scenes and live
state-stack transitions retain their prior handling.

- ~~Distinguish ordinary death from objective failure at the restart boundary.~~
- ~~Exercise gas, death, fade, restart request and checkpoint restoration.~~
- ~~Verify restored health, position, weapon, ammunition and forward execution.~~
- ~~Cover unknown player identity, overkill health, early fade and successful scenes.~~

The opt-in aftermath gate invokes the same original throw callback as before,
then continues beyond projectile unlink. It checks that the original failure
flag stays clear, that the host publishes the restart command in state 2, and
that the actual checkpoint restores its health, position and full inventory.
The G4 campaign gate additionally enters original damage routine FUN_80069cb0
with environmental damage and exercises the original death controller in every
mission; existing objective-failure and success checks remain separate.

Follow-up Release verification:

- Portable standard tests: **24/24**. PsyCross: **28/28**, including OpenGL.
- **21 original-image gates** passed across the enabled run and the targeted G4
  rerun. The full CTest run passed its other **44** tests; after correcting the
  newly added hit fixture to use original reaction type 0x0f (type zero has no
  death animation), the G4 command passed the complete campaign.
- G4: **20 ordinary deaths**, **20 objective failures**, **20 successes** and
  **20 EOL stage/finalize pairs**, with coherent checkpoint/inventory restoration.
- CATACOMB aftermath: original death at update **8**, state-2 checkpoint request
  at **90**, objective-failure flag **0**, restored health/position/inventory and
  eight subsequent updates verified.
- Campaign grenade transforms: **19 cases / 18 missions**, **857** exact MATRIX
  samples. The separate PAD charge/release/quick-click fixtures remain green.

The artificial throw is used to reproduce the original gas/death controller;
a full manual playthrough is still outside these automated contracts.

## Manual comparisons still required

- Chase and first-person combat at full/low cover: standing, crouched, peeking,
  exposed head/shoulder, doors changing state, and enemies reacquiring after
  actual exposure. Repeat with the flashlight on/off.
- Run/walk/crouch/roll, stairs, ladders, ledges, falling and moving platforms in
  matching PSX scenes, including transitions into/out of aim and radio.
- Fire, reload, empty-magazine auto-reload, quick/wheel/direct weapon selection,
  taser hold/release, grenade/gas victims, scanner, flashlight, key cards, C4 and
  antigen at their authored targets.
- Complete boss encounters, escorts/protected actors, CATACOMB's full tail,
  all timed/stealth parameters and both success/failure branches.
- Restart checkpoints during combat, after interactions, after pickups and
  around scripted cameras; check inventory, doors, actors and objective state.
- Compare particles, HUD/optic overlays and gameplay feedback at the exact same
  moments. Presentation details are tracked in the
  [presentation audit](PSX_PARITY_AUDIT.md).

These checks stay open until observed; successful automated gates are not
labelled as manual completion. PC mouse/control preferences, Agent rules,
widescreen and interpolation remain explicit port features.

The [November 18 pickup/menu follow-up](PSX_PICKUP_MENU_FIXES.md) records the
weapon/armour/C4 box presentation and restored menu-opening sound.

## Reproduction

~~~powershell
cmake --preset windows-msvc -DSF_BUILD_ROM_PROBES=ON '-DSF_SUPPORTED_ROM_CUE=<supported.cue>'
cmake --build --preset windows-release
ctest --preset windows-release --output-on-failure -j 3
# Default grenade subset, one mission, or the complete original grant matrix:
& .\build\windows-msvc\Release\sf_retail_grenade_probe.exe '<supported.cue>'
& .\build\windows-msvc\Release\sf_retail_grenade_probe.exe '<supported.cue>' 7
# Isolate the original callback in one allowed mission:
& .\build\windows-msvc\Release\sf_retail_grenade_probe.exe '<supported.cue>' 13 callback
# Continue through gas/death/fade and restore the checkpoint:
& .\build\windows-msvc\Release\sf_retail_grenade_probe.exe '<supported.cue>' 13 aftermath
& .\build\windows-msvc\Release\sf_retail_grenade_probe.exe '<supported.cue>' all
# Return to the normal portable build:
cmake --preset windows-msvc -DSF_BUILD_ROM_PROBES=OFF -DSF_SUPPORTED_ROM_CUE=
cmake --build --preset windows-psycross-release
ctest --preset windows-psycross-release --output-on-failure
~~~
