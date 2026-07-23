# G4 findings

G4 connects the native title, FMV and UI host to the guest-authoritative retail
campaign without adding a second gameplay implementation.

## Production flow

- `--game <game.cue>` and the positional `<game.cue>` form start the complete
  release campaign. `--title-test` remains a compatible development alias and
  `--scene-test` remains a single-mission diagnostic mode.
- Retail mission failure restores the exact guest checkpoint. Retail success
  stages the completed mission durably, plays its EOL natively, then advances to
  the next SOL/briefing/gameplay package.
- Save V3 records a pending EOL transaction. Closing the process or failing the
  decoder during an ordinary or final EOL resumes that movie before progression.
  V1 and V2 saves migrate forward.
- Saves use a validated primary/backup pair. A corrupt pair can recover from the
  legacy disc-adjacent save, and write failure presents Retry, Continue without
  saving and Return to title choices.

## Gates

- All 20 mission packages reach stable production gameplay.
- All 20 retail failure callbacks produce exactly one checkpoint restart.
- All 20 retail success callbacks produce exactly one ending request and one
  staged/finalized campaign transition, including final completion.
- The catalog gate validates all 20 mission resources and 13 distinct overlays.
- The FMV gate decodes all 29 cataloged STR files: 17,135 video frames and
  43,221,024 stereo sample frames with monotonic timestamps and non-empty output.
