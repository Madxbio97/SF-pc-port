# G3 retail gameplay restoration

G3 keeps native input and presentation while the USA v1.1 guest remains the
only owner of mission scripts, terrain triggers, actor lifetime, AI, combat,
stealth, objectives and terminal outcomes.

The abbreviated hybrid bootstrap skipped the software side effect of retail
`FUN_8001629c`: gameplay latch `0x80116962` remained zero. Consequently
`FUN_800830ac` rejected every authored terrain-volume trigger. The bootstrap
now restores that exact post-loading latch after the native graphics reset.
It does not synthesize an actor, clear a dormant flag, or dispatch visibility
event 6.

The production Kravitch route is the natural G3.2 regression: normal PAD
movement crosses the authored SUBWAY volume, retail event 12 clears the dormant
state, and only then may the exact guest display node, AI record and complete
15-part pose enter native presentation. The gate also rejects early visibility,
Gabe-root substitution and T-pose fallback.

G3.3 validates all 20 mission packages. Its exact matrix covers 55 named or
mission-critical actors and 23 overlay activation sources, including Kravitch,
Aramov, Girdeux, Hans, Benton, both Phagan variants, Mara, Vladimir, the
Chopper, scientists, infected scientists, the CATACOMB doctor and Mei,
Richard, SOELITE and Kane. Dormant actors stay hidden until guest logic owns a
complete pose; checkpoints restore their lifecycle exactly.

G3.4 validates an exact authored actor in every mission. Naturally active
actors exercise guest awareness, targeting, animation, nonlethal damage and
death; trigger-gated or remote actors must instead preserve their exact
inactive instance/AI-controller contract without early presentation. Hans and
the Chopper use their retail rigid-boss handlers rather than the common HMD
damage layout. No room/player teleport, object write, spawn event 6 or direct
RAM fallback is used. Every exercised scenario replays exactly from one guest
snapshot. CATACOMB has a separate PAD-only stealth gate: the complete five-room
route topology is validated, its first natural follow edge keeps the doctor AI
and full pose live, and natural exposure reaches the retail failure callback.

G3.5 supplements the common all-mission objective/success/failure regression
with overlay-owned callbacks. It covers 21 deterministic scenarios: Kravitch
and the radio, Aramov, Girdeux, Hans, protected Phagan/Mara, the Chopper,
scientist and viral-host groups, CATACOMB's doctor/Phagan/Mei branches, and
Richard. These are test seams into the original callbacks, not production host
mission logic; full RAM and mission/actor state must replay exactly.

Supported-ROM gates:

```text
sf_g3_gameplay_probe <game.cue>
sf_g3_retail_spawn_probe <game.cue>
sf_g3_special_actor_probe <game.cue>
sf_g3_ai_combat_probe <game.cue> [mission-index]
sf_g3_stealth_probe <game.cue>
sf_g3_overlay_outcome_probe <game.cue>
```

Interactive levels are one-based in the launcher and command line:
`--mission=1..20` or `--level=1..20`; add `--no-launcher` for direct startup.
