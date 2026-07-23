# H5 campaign findings

H5 completes the hybrid-emulation migration for the supported `SCUS-94240`
USA v1.1 disc. The retail campaign contains 20 mission resources backed by 13
distinct overlays. Shared-overlay mappings and SOL/EOL STR paths are explicit in
the mission catalog and verified against the original image.

`MissionPackage`, `LegacyMissionImage`, `LegacyGameplayVm` and the production
runtime now accept any catalog mission. Retail FOG variants with unsorted extents,
optional empty entries and repeated DAT prefetch separators are handled without
weakening overlap or bounds checks. The GTE DPCS command required by later mission
render paths is implemented.

The production title host loads missions on demand. A successful guest terminal
state plays the mission's EOL movie natively when present, loads the next package,
plays its SOL movie when present and resumes guest-authoritative gameplay. A guest
failure completes the retail fade and restores the captured guest checkpoint.
The Georgia Street mid-mission `INTRO.STR` handoff remains native and preserves the
live guest session.

Automated supported-ROM gates:

```text
sf_h5_catalog_probe <game.cue>
sf_h5_campaign_probe <game.cue> [mission-index]
sf_h5_bootability_probe <game.cue> [mission-index]
```

The release campaign starts with `--game <game.cue>` or a single positional
`<game.cue>`; `--title-test` remains a development alias. Interactive mission
selection is available in the launcher for campaign/title and scene modes. The
equivalent one-based CLI forms are `--mission=1..20` and
`--level=1..20`; combine either with `--no-launcher` for direct startup.

The catalog gate validates all 20 resources, 13 overlay mappings and movie files.
The campaign gate loads and boots every mission, advances a guest outer frame,
drains PCM, captures/restores a checkpoint, requires bit-exact replay, verifies
immutable monotonic presentation frames, constructs the production gameplay
session, and exercises actual retail success/failure RAM latches through the
production transition classifier.

The bounded G1 bootability gate never stops at the first broken level. It reports
all 20 missions independently, names the exact bootstrap or runtime phase on
failure, and requires a consumable production presentation frame followed by a
monotonic guest update. Set `SF_SUPPORTED_ROM_CUE` while configuring CMake to add
this legally supplied ROM gate to CTest.

Final gate results: headless CTest 12/12, PsyCross CTest 14/14, H5 catalog 20/20,
and H5 campaign 20/20 with 20 exact replay frames and 44,218 PCM frames. H2 CD/DMA3
and continuous-loop, H3 audio and H4 presentation ROM gates also remain green.

An interactive full campaign playthrough remains release acceptance rather than
an automated migration dependency.
