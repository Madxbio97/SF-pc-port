# PSX presentation parity audit — 2026-11-16

Reference: the supported USA v1.1 image, executed through the integer R3000/GTE.
This audit compares recovered presentation contracts. It does not certify an
identical complete framebuffer or a complete mission playthrough.

The [November 17 cover/stealth follow-up](PSX_OCCLUSION_FIXES.md) records the
subsequent Girdeux, visual-acquisition and crouched-target corrections. The
[gameplay audit](PSX_GAMEPLAY_PARITY.md) adds the corrected skeleton-space cover
samples and campaign-wide behavioural/weapon gates.

## Completed in this pass

- ~~Recover all three sprite sorting paths, including the regular sorter's
  bit-27 rectangle shortcut.~~
- ~~Preserve the rectangle pivot while ignoring scale/rotation in that shortcut.~~
- ~~Apply horizontal/vertical UV flips only on the original FT4 path.~~
- ~~Truncate Q12 degrees toward zero before supplying the GTE rotation angle.~~
- ~~Keep authored priority zero for camera-list UI sprites.~~
- ~~Preserve the last texture row/column and avoid applying the half-texel
  correction twice to explicit retail sprite coordinates.~~
- ~~Keep precise screen sprites in the UI filter/depth class.~~
- ~~Run all-mission sprite, environment and lighting gates and both Release
  test suites.~~

### Original evidence

USA v1.1 entry points:

| Entry | Original behaviour | Native correction |
| --- | --- | --- |
| `GsSortSprite`, `0x800e82b4` | Bit 27 selects SPRT; subtract `mx/my`, ignore transforms/flips | Separate mapped rectangle from the FT4 transform |
| Same entry | Unit scale, zero rotation and no flips also select SPRT | Preserve the original shortcut |
| Same entry, FT4 branch | UV endpoints are `u+w-1/v+h-1`; flips swap those endpoints | Restrict flips to this branch |
| Same entry → `RotMatrix`, `0x800ebc04` | Raw rotation divided by 360, truncated toward zero | Use the same integer angle |
| `GsSortFastSprite`, `0x800e87b4` | SPRT at `x/y`; ignore pivot, transforms and flips | Keep independent fast-sort semantics |
| Both sorters | Priority zero is a valid OT slot | Remove the native minimum-one clamp |

The GPU adapter previously inferred exclusive UV endpoints from axis-aligned
screen geometry even when the source used inclusive retail endpoints. It could
shrink the atlas bounds twice and shift the sample a second time. Guest sprites
now attach their final relocated UVs and inclusive bounds to the exact vertex
batch. Rectangle texel edges and transformed FT4 texel centres remain distinct.
Screen-space records have zero projection and keep the UI filtering class.

Primary implementation:
[`legacy_bridge_types.hpp`](../include/sf/game/legacy_bridge_types.hpp),
[`psycross_scene_world.inc`](../src/platform/psycross_scene_world.inc),
[`PsyX_GPU.cpp`](../external/PsyCross/src/gpu/PsyX_GPU.cpp).

### Verification

- Release portable: **24/24**.
- Release PsyCross: **28/28**, including the hidden OpenGL subsystem test.
- New opt-in `sf_retail_sprite_probe`: **20 missions**, **3,246 observed original
  sorter calls**, **365 sampled original parameter sets + 578 generated edge
  cases = 943 replays**, **zero mismatches**.
- The ROM probe checks sorter/primitive selection, rectangle XY and dimensions,
  FT4 UV corner ordering, visibility rejection and the angle passed to the
  original `RotMatrix`. It does not compare floating-point FT4 vertex positions
  against fixed-point rasterization.
- Production adapter fixture: **16 cases**, zero mismatches in submitted
  coordinates/metadata, world versus UI depth, inclusive texture bounds and
  priority zero. Warm storage stayed stable during 10,000 camera updates.
  The local CPU cost was about **3.1 microseconds per sprite**; this is not a
  complete-frame performance measurement.
- Environment gate: **20 missions × 241 frames**, **38,281 sprite samples**,
  all matched to authored assets, zero unmatched or ambiguous samples.
- Lighting gate: **57,344 samples across 20 missions**, zero mismatches against
  original instructions, zero unmatched projected vertices.
- The longer environment run exposed a stale diagnostic whitelist: it rejected
  the already-supported textured glass triangles in WHOUSE at frame 97.
  The diagnostic now uses the shared publisher/VM packet-format contract;
  the full all-mission run then passed.

The probe is disabled in normal builds. To reproduce with a legally supplied
supported image:

```powershell
cmake --preset windows-msvc -DSF_BUILD_ROM_PROBES=ON
cmake --build --preset windows-release --target sf_retail_sprite_probe
& .\build\windows-msvc\Release\sf_retail_sprite_probe.exe '<path-to-supported.cue>'
cmake --preset windows-msvc -DSF_BUILD_ROM_PROBES=OFF
```

No ROM content, live memory captures, personal settings or saves are distributed.

## Remaining comparison work

- Matched PSX/PC scene comparisons through later rooms and scripted camera
  changes: fog, lighting falloff, transparency overlap, near-camera surfaces.
- Particle appearance during glass destruction, grenade/gas explosions, snow,
  blood and shell ejection, including effects spawned after mission startup.
- UI checks at 4:3, 16:9 and 21:9: timer, radar, weapon selector, messages,
  pause panels and optic overlays in their active states.
- Manual behaviour checks for combat, stealth, scripted objects, mission
  transitions and checkpoints. Gameplay testing remains with the user.
- Complete-frame stutter measurements in repeatable moving-camera scenes.

Previously requested PC presentation features (widescreen, smooth geometry,
darker grading, controlled dithering and optional effects) remain deliberate
differences. They need a separately selected reference preset before a strict
pixel-for-pixel PSX image comparison can be meaningful.
