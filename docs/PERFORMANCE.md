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
