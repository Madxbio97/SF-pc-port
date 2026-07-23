# Contributing

Contributions are welcome when they preserve the project's legal boundary and
the original gameplay behavior.

## Before opening a change

1. Search existing issues and document the affected mission and runtime mode.
2. Keep original game data out of the repository, patches and test fixtures.
3. Prefer a focused change with a reproducible before/after description.
4. Add a deterministic test when the affected system can be tested headlessly.

## Build and test

Follow [docs/BUILDING.md](docs/BUILDING.md). At minimum, a code change should pass:

```powershell
cmake --build --preset windows-release
ctest --preset windows-release
```

Renderer/platform changes should also pass:

```powershell
cmake --build --preset windows-psycross-release
ctest --preset windows-psycross-release
```

Interactive validation is manual and must state the disc revision, mission,
graphics settings and GPU. Do not automate a game launch in tests.

## Code style

- Match the surrounding C++ style and keep warnings clean.
- Keep platform APIs behind the existing host/backend boundaries.
- Preserve deterministic guest timing and immutable presentation snapshots.
- Avoid guessed guest-memory writes or native gameplay replacements.
- Keep reverse-engineering notes in `docs/` and implementation comments concise.
- Run `git diff --check` before committing.

## Bug reports

Include:

- Public-test version or source commit.
- Mission number and checkpoint/area.
- Exact reproduction steps.
- Resolution, aspect, filtering and MSAA settings.
- GPU and driver version.
- Screenshot/video and the generated `.log` file when relevant.

Never attach the original BIN/CUE image, extracted retail files, save data with
personal information, credentials or unrelated crash dumps.

## Licensing

By submitting a contribution, you agree that project-owned code is distributed
under the repository's MIT license. Do not submit material you do not have the
right to redistribute.
