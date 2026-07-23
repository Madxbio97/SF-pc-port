# Troubleshooting

## Does the game require the original image?

Yes. A legal BIN/CUE image of the USA v1.1 release (`SCUS-94240`) is required at
runtime. The repository and release archive contain no game data.

## The launcher rejects my image

Confirm that the CUE references the correct BIN filenames and that all files are
present. Other regions or revisions have different executables/overlays and are
not supported. The expected PS-X executable SHA-256 is:

```text
bac292061ad5bc718ce137ef5b43d3d7e9b1b65248fb0d52229f328ccfe4ab4e
```

## Failed to create native PSX framebuffer

If the log contains `PsyX_EnsureNativeFramebuffer`, choose a smaller internal
resolution, disable MSAA, update the GPU driver and retry. Very large resolutions
and high MSAA multiply color/depth allocation requirements. Also confirm the GPU
supports the required OpenGL 3.x feature set.

## Missing DLL or immediate startup failure

Extract the complete ZIP. Do not move only `syphon_filter.exe`; its SDL2, OpenAL,
FFmpeg, fmt and Visual C++ runtime DLLs are packaged beside it. Antivirus
quarantine should be checked before replacing files from the verified archive.

## The DOSSIERS window is empty

Keep `assets/dossiers/screens/dossier_01.png` through `dossier_04.png` in the
packaged directory structure. Re-extract the verified archive if they are missing.

## Mission selection is locked

This is expected on a clean profile. Missions unlock sequentially from campaign
progress. A save on mission N unlocks missions 1 through N. The developer override
requires a manually created `syphon_filter_cheats` marker beside the executable.

## I need a clean profile

Close the game and back up, then remove or rename:

```text
%LOCALAPPDATA%\SyphonFilterPC
```

This resets saves and launcher settings. It does not affect the selected game
image itself.

## Build configuration cannot find vcpkg

Set `VCPKG_ROOT` before running the PsyCross preset:

```powershell
$env:VCPKG_ROOT = 'D:/Tools/vcpkg'
cmake --preset windows-psycross
```

If the build directory was configured with a different path, remove only
`build/windows-psycross` and configure it again.
