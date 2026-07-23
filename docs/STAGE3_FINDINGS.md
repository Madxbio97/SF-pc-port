# Stage 3 findings: Georgia Street bootstrap

All names are descriptive clean-room names recovered from data flow, referenced
strings and original disc metadata.

## Mission identity

The executable's mission-name table maps index `0` to `Georgia Street`. Its
parallel resource table maps the same index to `SUBWAY`, establishing the first
mission contract:

- FOG archive: `FOG/SUBWAY.FOG`
- mission overlay: `SUBWAY.OVL`
- opening movie: `SOL/SUBWAY.STR`
- ending movie: `EOL/SUBWAY.STR`

## INIT.OVL load contract

`INIT.OVL` loads at `0x8014c0a8`; its initialization entry is `0x8014d7ac`.
The recovered routine mounts `FOG/<resource>.FOG`, loads the selected overlay at
`0x80146630`, then opens these virtual paths inside the mounted FOG:

```text
<resource>/SLF.RFF
<resource>/VLF.RFF
<resource>/DLF.RFF
<resource>/VRAM/<resource>.HOG
<resource>/WLDEMD.HOG
<resource>/<resource>.BIN
<resource>/<resource>.DAT
```

The native package loader keeps the original physical layout while exposing the
same resources through typed archive objects.

## FOG container

The archive uses one 2048-byte table sector followed by sector-aligned files:

```text
u32 flags
u32 declared_sector_count
u32 reserved[2]
struct entry {
    char name[16]
    u32 start_sector
    u32 sector_count
} entries[]
```

The parser validates header fields, ASCII names, duplicate names, ordered and
bounded extents, and the declared archive end. It ignores only bytes beyond that
declared end, which are present in the ISO file but not part of the mounted FOG.

`SUBWAY.FOG` contains 14 files. Its `VRAM.HOG` contains 33 texture resources and
`WLDEMD.HOG` contains 91 world-model resources. New Game now mounts and validates
these resources before playing the original Georgia Street opening movie.

## Native terrain path

`WLDEMD.HOG` contains 91 EMD models. Across Georgia Street they contain 1,195
sections, 32,324 vertices and 22,268 compact terrain polygons. The native parser
validates every section boundary and polygon vertex reference. It reconstructs
the original triangle/quad indices, UV expansion, CLUT selector and texture page.

`VLF.RFF` starts with a 32-bit texture-page mask. Each selected bit contributes a
64x256 16-bit VRAM page; the final 256x32 block is uploaded to the mission CLUT
area at `(768,480)`. This is now paired with the EMD renderer through PsyCross's
ordering-table path, preserving PGXP cache data. PGXP texture correction, PGXP
Z-buffering and nearest-neighbour sampling are enabled for this scene viewer.

The EMD vertices are already stored in mission-global coordinates. All 91 models
are parsed at mission construction, but only the current room and the active
visibility set recovered from `SUBWAY.DAT` are submitted each frame. Georgia
Street starts in room `73`; its initial active terrain set is
`73, 74, 67, 83, 75, 76, 82, 81, 72`.

The texture streamer combines shared `VLF.RFF` pages with the required room bank
from `VRAM.HOG` or `VRAM1.HOG`. State is tracked by physical VRAM slot so logical
pages `0..5`, which relocate onto slots `6..11`, cannot leave aliased textures
incorrectly marked as resident. The CLUT is switched with its dynamic bank.

Terrain submission rejects back faces using the same projected winding sign as
the original NCLIP path. Polygons whose compact material word is zero remain
available to collision queries but are not rendered. Together with the PGXP
depth buffer, these rules prevent the inner and outer layers of walls from being
drawn through one another.

## Mission objects

`SUBWAY.BIN` describes 91 room-object lists, 355 instances and 61 object
definitions; object `83` is the player. Each definition names a logical TMD
resource, while `INIT.OVL` resolves that stem by trying GMD, EMD and HMD data.
The native loader follows the same resource choice across GMD, EMD and HMD.

The first two words of `DLF.RFF` delimit an embedded 94-entry HOG. It contains
82 compact GMD resources, seven EMD resources and five animated HMD resources.
GMD supplies static props and planar effects; EMD supplies larger static objects.
Both formats are transformed by each instance's 3x3 fixed-point matrix and
mission position. Active object lists are the deduplicated union of the exact
room lists belonging to the active terrain set.

At the initial spawn this produces 30 renderable object instances plus the
player. The visible set includes five `GLIT` effects, four `CFIREA` emitters,
two `SMOKE` effects, CHEMO, TERRO and
the bar/checkpoint/lighting props. Compact GMD texture pages, CLUT selectors,
semi-transparency and packed 10/10/12-bit vertices are decoded directly. Planar
semi-transparent effects are treated as two-sided; ordinary props retain
back-face rejection.

`CFIREA.GMD` is an emitter marker, not the visible flame mesh. Its reserved TP12
tile becomes an opaque box if the marker is submitted directly. The recovered
class-0x30 event path starts two camera-facing particles at the authored object
origin, with the native 0x70 vertical bias, size-0x999 projection (a 456-unit
EXPL quad), bit-mask jitter, acceleration, damping and 7..14-tick lifetime. The
port loads `EXPL000.TIM` through `EXPL007.TIM` from `COMMON/SPFX.HOG` and restores
their shared page-28 texture and row-511 CLUT after mission texture streaming.
Particles render in a separate additive pass: opaque PGXP depth hides them behind
walls, cars and props, while they test but never write depth. A small contact bias
prevents street flicker. Each intact particle is one quad; a near-plane crossing
clips its complete perimeter before triangulation, preventing half-billboard
dropout. The original gameplay path advances at 30 Hz. The port polls input and
collision at 60 Hz but distributes each 30 Hz simulation step across two updates.

Police cars use `CP.EMD`, `CPTOP.GMD` and opaque `LIGHT.GMD` geometry. The lightbar
does not use additive proxy sprites: its native four-frame sequence copies two
16-word by 32-line TP10 cells every two 30 Hz gameplay ticks. Reapplying the
current phase after texture streaming keeps the red/blue pulse stable across room
changes. Since `LIGHT.GMD` remains in the normal opaque pass, walls occlude it
through the same PGXP depth test as the rest of the car.

The HMD reader validates the 15-part hierarchy, padded vertices and normals,
triangle vertex strides, bind transforms, UVs, direct GPU CLUT/tpage selectors
and per-part bounds. `COMMON/PCHAN.HOG` supplies the original compressed
15-part animation channels. The bounded decoder handles all four original
rotation encodings, absolute local translations, split upper/lower masks and
full-body clips. Gabe selects the original `ST0/ST02`, `WK0` and `RN0` standing,
walking and running channels; CHEMO and TERRO run phase-offset `IDLE13` cycles.
The decoded root prefix supplies signed `{x, y, z, pad}` records. `WK0.LWR`
travels 159 world units over 25 frames and `RN0.LWR` travels 307 over 14; each
record is split evenly across two 60 Hz updates while its matching PCHAN pose is
held, preserving the original 190.8/657.86 world-units-per-second rates.
The stored HMD parent-to-part bind transforms are inverted (`R^T`, `-R^T t`),
while decoded PCHAN matrices are composed as local child poses. Referenced
geometry keeps native +Y down and is rebased from its maximum Y to the actor's
ground point before the mission transform is applied.

## Current gameplay slice

The scene now has a softly damped over-the-shoulder chase camera that stays behind
Gabe without rigidly pinning the view, switches to first-person only for manual
aiming and clamps against active EMD walls. Gabe uses original locomotion and
action root motion; enemies animate; proportional analog and digital movement,
turning, running, strafing, terrain grounding, provisional wall collision, room
transitions and corresponding terrain/object/texture-set rebuilds are connected.
New Game reaches it after the original Georgia Street opening movie;
`--scene-test` enters it directly.

## Premission briefing state

The original title does not enter gameplay directly after `SOL/SUBWAY.STR`.
`INIT.OVL` builds a 310x170 text panel, shows the mission descriptor, then pushes
system state 8. That state accepts Cross and pulses `Press %x to continue` every
16 ticks between black and RGB `(200,200,255)` before releasing the mission.

The native path now follows the same visible sequence:

```text
New Game -> SOL/SUBWAY.STR -> mission briefing -> fresh confirm -> gameplay
```

The location, mission title, time and both directive paragraphs are parsed from
the original `DLF.RFF` data block. The panel uses the recovered RGB
`(110,130,200)`, dimensions and prompt cadence. Its input gate requires the FMV
skip/confirm button to be released before accepting a new press, so the input
cannot leak into gameplay and trigger the scene exit binding.

The slice deliberately stops short of claiming playable mission parity. Original
Gabe locomotion, stance, aim, fire, reload, draw, roll and transition clips are
connected to weapon selection, manual/automatic aim, hitscan fire and ammunition.
Enemy AI, contextual doors, triggers, objectives and checkpoint state are not
recovered yet. CFIRE is animated; the remaining effects still use their static
geometry. Terrain collision remains approximate. Terrain and static objects
crossing the near plane are clipped before GTE submission; generated vertices
preserve the source UV and color interpolation and receive normal PGXP projection
data.
