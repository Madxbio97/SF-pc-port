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
