# Performance validation

## Target

The presentation target is 240 FPS on recorded reference hardware, corresponding
to a 4.17 ms frame budget. Retail gameplay simulation remains fixed at 20 Hz.
This is a reproducible regression target, not a guarantee for every GPU or
resolution.

## Reference configuration

Use a PsyCross Release build with the same resolution, MSAA and filtering
settings before and after a change. Disable vertical synchronization and select
**Unlimited** for throughput measurements. Press **F6** to display presentation
FPS, logic rate and average frame time.

Record the commit, CPU, GPU, driver, Windows version, resolution and settings.
External frame-time capture is preferred for percentile results; the smoothed
in-game counter is a sanity check.

## Protocol

1. Start a new process for each run.
2. Reach the specified scene and warm it for 60 seconds.
3. Capture the following 60 seconds.
4. Repeat three times and report the median run.
5. Compare revisions only on the same machine and settings.

## Reference scenes

| Scene | Coverage |
| --- | --- |
| Georgia Street opening | World submission, streaming, fire and police lights |
| A busy combat encounter | Actors, shadows, combat and transient effects |
| Tunnel or Kazakhstan searchlight area, normal and SVD aim | Moving lights, fog, optics and HUD |

Record an exact save/checkpoint and camera path with the result so the same scene
can be reproduced. Capture standing and a slow 360-degree sweep where possible.

## Acceptance criteria

A revision passes only when every scene meets all criteria:

- Average presentation rate is at least 240 FPS uncapped.
- 95th-percentile frame time is no more than 4.17 ms.
- 99th-percentile frame time is no more than 5.00 ms.
- No post-warm-up hitch exceeds 16.67 ms.
- The F6 overlay reports `LOGIC 20`.
- Rendering, audio and gameplay remain unchanged.

After the uncapped pass, repeat one run with a 240 FPS limit. The counter should
remain within 238-242 FPS without periodic spikes. A change is also a regression
if any scene worsens median or 95th-percentile frame time by more than 5% on the
same hardware, even when it still passes the absolute target.

Run `ctest --preset windows-psycross-release` after every performance change.

## 2026-11-16 CPU vertex submission measurement

Baseline: `429a3a8`. Hardware: Intel Core i9-12900F, NVIDIA RTX 4080,
driver 617.42, Windows build 22631 (23H2), MSVC Release with IPO.

This is an isolated CPU measurement, not a completed reference-scene acceptance
run. It submits 24 copies of the same real posed Gabe HMD through the production
renderer while rotating the camera through 360 degrees. Each fresh process
warms 60 iterations and measures the following 720; three processes are run
sequentially. It performs no gameplay input, GPU draw, swap, VSync or frame cap,
so framebuffer resolution and filtering do not affect these numbers.

Median of the three per-run metrics, in milliseconds:

| CPU submission | Baseline | Optimized |
| --- | ---: | ---: |
| Mean | 13.142 | 6.661 |
| Median | 12.538 | 6.599 |
| 95th percentile | 15.115 | 7.208 |
| 99th percentile | 26.000 | 8.109 |

Each run submitted exactly 4,955,952 triangle packets including warmup.
The hash of every submitted corner's precise screen X/Y and depth is
`7e6d79b5c9319a5b` in all baseline and optimized runs.

Changes retain frame-local exact-coordinate tables by generation rather than
allocating/freeing map nodes every frame, and register HMD/clipped triangle
corners once at their final addresses. Address precedence and ambiguous
packed-coordinate handling remain unchanged. Nested `PsyX_BeginScene` calls
from HUD primitives also no longer repump SDL events after input sampling;
explicit frame input updates and new-scene polling remain active.

Regression checks cover table growth at 24,000 distinct records, repeated
generations, copied and ambiguous vertices, close clipped surfaces, actor
self-depth with MSAA, and the real shader atmosphere gradient. A queued SDL
test event verifies that a nested primitive cannot consume new input.

Full scene frame-time percentiles, presentation pacing and subjective movement
smoothness still require the user's manual reference-scene runs above. The
approximately 49% CPU improvement here must not be presented as a 49% whole-game
FPS increase.

## 2026-11-16 all-mission lighting and CPU cost

The opt-in `sf_retail_lighting_probe` observed original renderer calls with
mission sources and with the flashlight enabled in all 20 missions, then
replayed `FUN_800d3b8c/FUN_800d3cb4` through the integer R3000/GTE. All 57,344
sampled packed colours matched; no source vector was unmatched. Observed light
projection H was 38 throughout. A separate native-presentation check confirmed
that this guest H reaches lighting on every mission independently of display
camera H=261. These are initial-scene automated checks, not a manual walkthrough
of every room, script state or camera transition.

CPU-only rotating-camera measurement on the same i9-12900F reference machine
compares `1e55bd4` to shared vertex-light caching. It traverses actual active
EMD sections, including camera frustum rejection, interpolated light matrices,
live guest colours and the production polygon-lighting cache. Each process
warms 60 iterations and measures 720, with 12 presentation samples per guest
tick; three fresh processes run sequentially. Resolution, GPU filtering,
swaps and VSync are excluded from this isolated stage measurement.

Median of the three per-run metrics, in milliseconds:

| Mission | Mean before | Mean after | P95 before | P95 after | P99 before | P99 after |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Georgia Street (SUBWAY) | 0.571 | 0.215 | 0.692 | 0.263 | 0.821 | 0.319 |
| Romer's Base (BASEEXT) | 0.928 | 0.408 | 1.112 | 0.489 | 1.274 | 0.557 |
| CAVE2 | 0.388 | 0.119 | 0.418 | 0.138 | 0.500 | 0.188 |

A single control run with the former H=261 used 0.561, 0.914 and 0.371 ms mean
respectively. Restoring the wider original light footprint added only about
0.01-0.02 ms to this CPU workload; eliminating repeated shared-vertex work more
than compensates for it. No lighting-stage sample in the optimized runs
exceeded 1 ms. This does not establish full-frame hitch percentiles.

All before/after colour hashes and polygon/vertex counts were identical:

| Mission | Colour hash | Polygon visits including warmup |
| --- | --- | ---: |
| SUBWAY | `fb0e5f30c3ed476f` | 1,578,078 |
| BASEEXT | `3862dd6d82f0ff80` | 997,750 |
| CAVE2 | `48dfbaab949f932d` | 1,125,563 |

The cache is reset per section pass, retains allocation capacity and keeps
different vertex indices distinct even when their positions coincide.
Portable tests cover reuse, changed colours, section changes and growth.
Full-frame smoothness remains a manual acceptance check under the protocol
above; these numbers are not a whole-game FPS claim.
