# H4 renderer/UI bridge findings

H4 keeps the supported target fixed at `SCUS-94240` USA v1.1.

The shipping first-mission runtime now has one gameplay authority: the guest. Its
only host-to-guest gameplay input is a PlayStation PAD sample. The former
`native_player` selector and the production path that wrote player pose, room,
inventory, damage, impacts or interactions into guest RAM have been removed.
Low-level writer helpers remain available only to development probes that compare
known retail functions; `GameplaySession` cannot reach them.

Each completed retail outer frame is projected into one
`shared_ptr<const LegacyPresentationFrame>`. It contains:

- a deep-copied renderer snapshot with camera, fade, player, object/bone and EXPL
  particle state;
- a UI snapshot with vitals, inventory, objectives, target lock and tracked threat
  commands;
- edge commands for present, UI refresh, checkpoint commit, intro/ending FMV,
  failure restart and runtime fault.

Renderer and UI carry the same guest-frame number and publication sequence.
`GameplaySession` accepts a frame only once, only when both presentation commands
are present, and projects renderer/UI state from that same immutable pointer.
PsyCross also rejects a missing or regressed submitted sequence. A bridge fault
publishes only `runtime_fault`; PsyCross returns to the title, the map fades to
black, and no native gameplay or guessed HMD pose is used. Checkpoint snapshot or
restore failure follows the same fault path instead of dropping the guest edge or
falling back to a native restart.

Automated gates:

```text
sf_legacy_presentation_bridge_tests
sf_h4_probe <game.cue>
```

The unit gate verifies atomic deep copies, consumer replay/split-tick rejection,
command deduplication, UI target/threat projection, invalid-slot rejection and
fail-closed fault frames. The ROM gate boots the original first mission, exercises
checkpoint capture/advance/restore and reset, then advances nine retail
presentation boundaries. It requires monotonic sequences, coherent render/UI
frames, the full retail object table and guest-materialized actor bone matrices.
It also exercises native mouse aim, quick switch, continuous/reversed and
multi-notch weapon-tape input, ordering, and the sniper scope against the exact
retail `FUN_800405f4` weapon state machine.

H4 itself does not implement SPU, XA or PCM; those remain H3-owned. The current
tree now supplies them through the H3 machine/runtime boundary, while H4 consumes
only immutable presentation state and native FMV commands.
