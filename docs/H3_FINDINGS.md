# H3 SPU/XA/PCM checkpoint

H3 keeps gameplay audio guest-owned. Native code only presents the emulated PCM;
FMV video and its decoded soundtrack remain native.

## Implemented source boundary

- A snapshot-safe SPU subset owns 512 KiB RAM, 24 voices, register MMIO at
  `0x1f801c00`, DMA4, IRQ9, ADPCM block/loop decoding, ADSR, CD input mixing and
  a bounded 44.1 kHz stereo PCM queue.
- DMA4 is connected through `PsxMachine` with a deterministic four CPU ticks per
  word. SPU RAM, registers, voices and fractional sample clock are serialized;
  already-rendered host PCM is deliberately cleared on restore.
- Raw MODE2/2352 sectors can be streamed from the CUE/BIN without loading the
  track into memory. Realtime Form2 XA sectors honor Setfilter, XA-enable and mute,
  bypass INT1/DMA3, and feed the decoder/SPU. Ordinary data sectors retain the
  existing FIFO, INT1 and DMA3 path.
- XA ADPCM supports 4/8-bit mono/stereo at 18.9/37.8 kHz with serialized predictor
  history. The retail 37.8 kHz path uses the PlayStation 6:7 zig-zag
  interpolator and serialized 32-sample rings when converting to 44.1 kHz.
- Each completed 20 Hz retail frame advances to a deterministic CPU/SPU clock
  boundary and normally emits 2,205 PCM frames. `GameplaySession` drains those
  frames into a bounded PsyCross/OpenAL queue. Gameplay and FMV sources share one
  reference-counted audio context, and gameplay output is reset at checkpoint,
  mission restart and FMV transitions.
- Retail sound code now executes in the guest; the previous game-audio no-op HLE
  hooks were removed. The PsyQ 120 Hz `SsSeqCalledTbyT` callback is registered
  through `InterruptCallback`, runs six times per 20 Hz gameplay update, and is
  serialized with the VM snapshot.

## Checkpoint regressions closed in source

- `advanceTicks(UINT64_MAX)` no longer performs billions of idle mixer chunks.
  CPU-to-SPU frame conversion and silent bounded-queue fast-forward are O(1).
- `PsxMachineState` heap-owns its large SPU snapshot and deep-copies it, avoiding
  the Windows stack overflow seen by the H4 ROM probe.
- Synthetic data-only MODE2 sectors retain the legacy neutral subheader, preserving
  the existing raw-header/INT1 contract.
- Synthetic FOG-member LBAs moved from `0x40000` to `0x50000`. The old base
  overlapped the physical `XA/INGAME.XA` extent and routed `SLF.RFF` sound-bank
  loads into XA sectors, leaving the guest object filled with `0x0c` poison.
- CD-ROM `Mute`/`Demute` now update XA routing through guest-issued commands.
- SPUSTAT exposes the hardware DMA read/write request bits and reports transfer
  busy only while DMA4 has a scheduled completion. BIOS `TestEvent` observes a
  pending transfer and `WaitEvent` advances to its exact serialized deadline, so
  sequential VAB loads can no longer overwrite one another.
- `ResetCallback` and `InterruptCallback(6, ...)` now preserve the retail sound
  sequencer lifecycle. This closes the zero-voice failure where sound banks were
  present in SPU RAM but `_SsVmFlush` never reached `SpuSetKey`.
- Checkpoint restore republishes the saved immutable presentation state without
  re-running guest bone resolvers. Future SPU output now replays bit-exactly from
  the checkpoint while the native presentation sequence remains monotonic.

## Automated targets

The source tree contains dedicated targets for raw-sector streaming, XA decode,
SPU behavior, SPU machine/DMA integration and CD-XA routing. The SPU suite includes
an explicit `UINT64_MAX` idle fast-forward regression; the VM suite covers the
20 Hz/2,205-frame audio clock, 120 Hz callback lifecycle, DMA event waits and
snapshot validation.

`sf_h3_audio_probe` is the supported-ROM gate. It boots USA v1.1, samples 400
guest updates, bounds the 44.1 kHz cadence, requires sustained nonzero PCM, and
performs a bit-exact audible capture/restore replay. The checkpoint result is
884,261 PCM frames, 883,468 nonzero frames, peak 26,356 and a 2,208-frame
bit-exact replay. Strict headless/PsyCross Release builds pass, CTest is 12/12
and 13/13, and the CD/DMA3, continuous-loop and H4 presentation gates remain
green on the same image.

## Deliberate limitations

- SPU pitch modulation, free-running noise, main/voice volume sweeps, ADPCM
  loop/repeat timing and the hardware pitch/address masks follow the DuckStation
  hardware algorithms and are regression-tested.
- XA predictor and resampler continuity survive retail file/channel/coding
  handoffs; Setfilter releases routing ownership without truncating queued PCM.
- Raw XA attachment is driven by the active retail CD stream and preserves the
  decoder until an explicit CD reset command.
- A huge elapsed interval with an active looping voice still requires real mixing;
  the O(1) path applies only when all voices and the CD input queue are idle.
- BIOS B0 still uses one synthetic event handle. SPU DMA `TestEvent`/`WaitEvent`
  are hardware-backed, but a general BIOS event table is not implemented.
