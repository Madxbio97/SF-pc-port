# Cover, stealth and flame follow-up — 2026-11-17

Reference: supported USA v1.1 executable and PARK2 overlay, replayed through the
integer R3000/GTE. These checks do not replace the user's gameplay testing.

## Completed

- ~~Confirm physical cover before an original positive visual acquisition.~~
- ~~Keep the observer's last-known position from updating through a wall.~~
- ~~Resolve Girdeux's live collision root when contact damage is dispatched.~~
- ~~Remove the dependency on a visible HANS model or a published hand pose.~~
- ~~Use the live PSX crouch flag for target height in vision, bullets, flame
  contact and the existing danger guard.~~
- ~~Check original flashlight-assisted acquisition with a retained target flag.~~
- ~~Run portable/PsyCross Release tests and original-instruction regressions.~~

## Changes and original boundaries

The previous flame guard sampled a presented right-hand transform before the
guest frame. It could become unavailable when the actor was outside the active
presentation, and it was already one publication behind the contact event.
Both exact PARK2 sites now resolve the current HANS instance (source 9,
class 0x3c) and the player collision root inside the event hook. They use the same
physical cover query as hostile bullets, including the extra check that a
shoulder sample has not crossed a thin wall from the player's centre.

The original acquisition routine, FUN_8005a2b0, selects its target and applies
retail range/FOV rules, then checks target.flags & 2. A retained positive flag
alone is insufficient to confirm current physical visibility. The native guard
runs at 0x8005a638, before the call to FUN_80059f4c that refreshes the observer's
last-known coordinates. On proven obstruction it clears only that flag and
takes the original zero-result branch. It neither replaces the whole AI routine
nor removes hearing, damage reactions, scripted alerts, or existing search
memory. Enemy-to-enemy targets pass through.

The crouch sample previously came from PlayerController::stance(), although
retail chase-mode synchronization can reset that controller to standing.
The query now carries the current player control flag 0x00100000 from guest RAM.
A local original-input diagnostic confirmed the standing -> crouch transition
(0x2 -> 0x180002 -> 0x100002) and the reverse transition. The check therefore
samples the lower body height while the player is actually crouched.

Gabe's reserved guest identity 0x29a is validated through the live player
pointer, separately from the authored NPC record count. Unknown actor identities
and unmatched instructions/events retain the original handling.

| Boundary | Contract |
| --- | --- |
| 0x8005a638, original branch 0x10400005 | Confirm only positive player visibility; preserve original range/FOV and negative results |
| 0x801480e4 and 0x801483ec return sites | Filter only the two proven PARK2 contact events |
| Original event 0x0d / mode 5 / id 0x29a | Keep unrelated event types, victims and overlay revisions untouched |

Flame animation can still continue while Girdeux fires into cover, as part of
the original state machine. This change blocks occluded contact damage; it does
not redesign the flame's particle simulation.

## Verification

- Portable Release: **24/24**.
- PsyCross Release: **28/28**, including the OpenGL subsystem test.
- CPU regressions cover aim modes 0..4, armed/unarmed observers, the reserved
  player slot, live crouching, original negative visibility, NPC-to-NPC targets,
  last-known-position updates, both flame sites and unrelated events.
- New opt-in sf_retail_occlusion_probe bootstrapped **20 missions**. Neutral
  startup provided observer samples in **13 missions**; the other seven had no
  active acquisition sample in this short window.
- **54** original-state replays include baseline samples and constructed
  retained-positive-target cases with the flashlight enabled through the original
  toggle. **21** positive player acquisitions were exercised; the production
  geometry query rejected **16** behind cover. Original negative/unrelated cases
  remained unchanged.
- Clear-path return values and the complete 2 MiB RAM image matched the original
  in all paired vision and flame replays. Both actual PARK2 call sites rejected
  blocked contact, and both were occluded at the sampled original spawn.
- Graphics regression: **943 sprite replays**, zero mismatches across the
  20-mission observer run.
- Lighting regression: **57,344 samples**, zero mismatches across 20 missions.

Reproduce the optional reference gate with a legally supplied image:

~~~powershell
cmake --preset windows-msvc -DSF_BUILD_ROM_PROBES=ON
cmake --build --preset windows-release --target sf_retail_occlusion_probe
& .\build\windows-msvc\Release\sf_retail_occlusion_probe.exe '<supported.cue>'
# Optional shorter contact-only check:
& .\build\windows-msvc\Release\sf_retail_occlusion_probe.exe '<supported.cue>' flame
cmake --preset windows-msvc -DSF_BUILD_ROM_PROBES=OFF
~~~

## Manual checks remaining with the user

- PARK2: place a column/wall between Gabe and Girdeux; repeat in chase view and
  first-person aim, standing and crouching, including after turning away from
  the boss and back.
- Stealth: stay behind a full wall and behind low cover while crouched, then
  expose the body. Check that actual exposure reacquires normally.
- Repeat with the flashlight on/off. Sound or gunfire may still legitimately
  alert enemies behind cover.
- Continue matched PSX/PC scene comparisons from the
  [presentation audit](PSX_PARITY_AUDIT.md), especially later scripted cameras,
  overlapping particles and widescreen UI states.

No complete playthrough or identical complete framebuffer is claimed.
