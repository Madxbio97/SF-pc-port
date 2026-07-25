# Architecture

The project is a hybrid PlayStation runtime. Ownership is intentionally one-way:

- Host: physical input, native renderer, native UI and native FMV playback.
- Guest: R3000A code, RAM, gameplay, animation, mission state and game audio.
- Bridges: host input becomes a PAD sample; guest state becomes immutable render/UI
  snapshots; FMV requests cross as commands without moving gameplay state to host.

There must be only one writer for gameplay state. Native code may observe guest RAM
through typed bridges, but must not overwrite player position, physics, combat,
inventory, room, AI or animation state.

- `sf_core`: errors, checked host-file I/O, hashes and platform-neutral geometry
  utilities.
- `sf_assets`: bounds-checked FOG/HOG/TIM ingestion plus EMD/GMD/HMD geometry,
  compressed HMD animation, mission-object and room-visibility decoding without
  a rendering dependency.
- `sf_disc`: CUE/BIN, ISO9660 and seekable raw MODE2/2352-sector access. Original
  assets stay on the user's disc image.
- `sf_psx`: PS-X EXE, R3000A/GTE execution, machine bus/IRQ/DMA/CD-ROM, SPU and XA.
- `sf_game`: supported-build identification, campaign mission packages,
  guest-runtime orchestration, the platform-neutral gameplay session and retail
  pause-map/menu presentation data.
- `sf_media`: optional FFmpeg-backed PSX STR/MDEC/XA decoder with host-neutral frames.
- `sf_psycross_backend`: optional PsyCross adapter. This is the only project-owned layer allowed to include PsyCross headers.
- `sf_tool`: development-time image inspection and extraction.

These dependency directions are executable policy, not only documentation.
`sf_architecture_check` rejects platform/PsyCross dependencies in the portable
layers and rejects project-owned `.cpp` files that are not assigned to a CMake
target. Development-only UI extraction is isolated in `apps/sf_tool/ui_export`
instead of adding disc/file-system responsibilities to the playable renderer.
The current cleanup inventory and remaining safe extraction seams are recorded
in [REFACTOR_AUDIT.md](REFACTOR_AUDIT.md).

The supplied DuckStation tree is CC-BY-NC-ND-4.0. It is used as a behavioral
reference only; its implementation is not copied or adapted without a separate
license from its author.

## Emulation boundary

The current `LegacyGameplayVm` executes the retail main executable and mission
overlays with HLE platform calls. Its interpreter is now attached to a width-aware
machine bus with edge-latched IRQ, COP0 exception entry/RFE, root timers, DMA/OTC,
a register-level CD-ROM controller and a serialized event scheduler. CD commands,
MODE2 data FIFO, IRQ2 and DMA3 are snapshot-safe; the file catalog is host-owned,
but overlay bytes enter guest RAM only through the hardware path. A snapshot-safe
SPU subset owns MMIO at `0x1f801c00`, DMA4 at four ticks per word, IRQ9 and a
deterministic 44.1 kHz mixer. Realtime MODE2 Form2 XA honors file/channel filtering
and mute, bypasses INT1/DMA3 and feeds the SPU; data sectors keep their FIFO/DMA3
path. Unclaimed registers retain a compatibility shadow while native GPU DMA
remains future work. GPU work is terminated at a native-render boundary, PAD is
supplied by the host and movie overlay requests are dispatched to the native
decoder. Disc assets stay on the user-supplied image.

Outside the native gameplay frame, one scheduler tick represents one interpreted
instruction or HLE call and device deadlines use CPU-clock ticks. The decoded
retail gameplay frame itself executes atomically at the stock PSX CPU clock;
SPU/CD/timers advance independently in exact 120 Hz slices. This replaces the
former guest overclock while preventing instruction-heavy retail scenes from
reintroducing console frame drops or holding sound behind a 50 ms gameplay block.
The VM can now resume its current PC without `beginCall`, stop before the native
GPU submit hook, present/pulse VBlank on the host and continue the same guest call
frame.
Pending CD and DMA events are part of VM snapshots and are validated one-to-one
against their device generations before restore. SPU RAM/voices/CD input and XA
predictor history are serialized; the bounded host PCM queue is excluded and
cleared on restore. The retail IRQ6 sound callback runs at 120 Hz (six guest
`SsSeqCalledTbyT` calls per 20 Hz update); its registration and absolute clock
anchor are serialized. Every 120 Hz slice is drained into a bounded
single-producer/single-consumer PCM ring. OpenAL Soft's callback-buffer extension
consumes that ring continuously, so a renderer stall cannot stop and repeatedly
restart the device source; an empty interval is emitted as silence and recovery
resumes from current PCM without replaying a stale queue. Startup waits for one
device update quantum and the ring is capped at roughly 70 ms, making latency
bounded at every presentation rate. The mixer remains fixed at 44.1 kHz: no
resampling, pitch correction or time stretching is applied. A queued-source
fallback remains for OpenAL implementations without the callback extension, and
finite UI cues use that path intentionally. All sources share one
reference-counted context with native FMV audio and are reset at checkpoint,
mission and FMV transitions. Restoring a checkpoint republishes its immutable
renderer/UI state without executing guest presentation helpers, so future PCM
replay remains bit-exact.

`syphon_filter --platform-test` remains a link/initialization smoke test.
`syphon_filter --game <game.cue>` (or the single positional `<game.cue>` alias)
is the release entrypoint. It streams the original startup movies and title HOG
from the supplied disc, runs the title update/render/input slice over the looping
title movie, and dispatches New Game into the complete campaign. Guest success
returns a native EOL request and advances to the next package; its SOL movie and
briefing are presented natively. Guest failure restores the guest checkpoint.
`--title-test` remains a compatible development alias for this host;
`--scene-test` intentionally bypasses campaign progression.

## Campaign runtime

`MissionPackage` mounts any of the 20 retail FOG resources plus
`COMMON/SPFX.HOG`, `COMMON/INTRFACE.HOG` and `COMMON/PCHAN.HOG`, including the
mission-local `MENU.HOG`, then exposes
terrain, textures, layout, instance data, special-effect frames, HUD/menu images,
actor channels and the embedded DLF object archive as typed assets. The gameplay
session parses all terrain models once, then derives the active terrain set from
the current room and mission DAT. It builds the active static-object set by
unioning the matching mission BIN room lists and resolving their logical TMD
stems to DLF GMD, EMD or HMD resources. The HMD bind hierarchy and original
standing, walking, running and enemy-idle animation tracks are implemented.

The PsyCross scene adapter owns only presentation concerns: fixed-point vertex
submission, ordering tables, physical VRAM-page residency, CLUT uploads, CFIRE
particle state, PGXP configuration and input translation. It renders active
terrain and transformed opaque objects into the world ordering table and shared
depth buffer. Fire uses a separate additive ordering table with depth tests and
depth writes disabled, so opaque walls occlude it without the street cutting into
the billboard. Back-face tests and non-renderable material filtering occur before
submission. Polygons crossing the camera near plane are clipped in world space;
fire clips its complete quad perimeter before triangulation. Generated vertices
then pass through the normal GTE/PGXP path. Collision still has access to the
complete parsed terrain.

Low-level VLF validation, physical page remapping and TIM uploads are isolated
in `psycross_vram`; the scene viewer consumes that interface and no longer owns
byte packing or raw `LoadImage` setup. Retail MENU.OVL map projection and pause
data construction live in `sf_game`, so the PsyCross backend draws an immutable
menu model instead of deriving gameplay/menu state itself. The recovered ACD
topology, fixed layout, PC-only extensions and text-flow invariants are recorded
in `PAUSE_MENU_PARITY.md`.

`GameplaySession` is a host presentation shell around the guest-authoritative
mission runtime. It translates physical controls to PAD state and projects
guest camera, actor matrices, inventory, vitals, target lock and danger into native
render/UI snapshots. `LegacyPresentationFrame` deep-copies both projections at one
guest tick and carries edge-triggered checkpoint/FMV/restart commands. The session
validates and consumes each monotonic sequence once before updating either native
projection, and PsyCross tracks the submitted sequence. Immutable bridge DTOs live
in `legacy_bridge_types.hpp`; presentation consumers therefore do not include the
VM, machine bus or R3000 runtime interfaces. The shipping
runtime exposes only PAD as a host-to-guest gameplay input; the former native-player
authority selector and player/room/inventory/damage injection path have been removed.

Renderer/UI bridge faults are explicit and fail closed. PsyCross returns to the title
instead of advancing native gameplay, and HMD actors without a guest bone snapshot
are omitted rather than animated from a guessed native clip. Development-time VM
diagnostic probes remain outside the shipping `LegacyFirstMissionRuntime`. The
PsyCross ordering tables use pre-budgeted frame-local packet storage. Allocation is
locked before the first raw packet address enters an OT, and capacity exhaustion
fails before a vector can relocate live packets. The PsyCross window controller still owns
resizable-window and borderless-fullscreen transitions without leaking SDL into
gameplay, and the native HUD/pause menu remain read-only consumers of guest state.
