# Release process

This checklist prevents local game data, saves, cheat markers and developer
artifacts from entering a public build.

## 1. Prepare the source tree

- Update `CHANGELOG.md`, the package-script default version and Windows resource
  metadata.
- Confirm `git status` contains only intended source/documentation changes.
- Confirm no BIN/CUE, save, log, dump, object, PDB or credential is staged.
- Verify the four dossier pages and multi-resolution launcher icon are present.

## 2. Build and validate

```powershell
cmake --preset windows-psycross
cmake --build --preset windows-psycross-release
ctest --preset windows-psycross-release
git diff --check
```

ROM probes are optional and require an explicitly configured legal image. Do not
run the interactive game as part of automated packaging.

## 3. Package

```powershell
$Version = '<version>'
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File tools/package_windows_release.ps1 `
  -Version $Version `
  -Configuration Release
```

Expected outputs:

```text
dist/SyphonFilterPC-<version>-win64/
dist/SyphonFilterPC-<version>-win64.zip
dist/SyphonFilterPC-<version>-win64.zip.sha256
```

The script fails instead of overwriting output or packaging a version without
`docs/releases/<version>.md`.

## 4. Audit the archive

- Verify the archive checksum and the internal `SHA256SUMS.txt`.
- Verify the packaged executable hash matches the built executable.
- Verify `locales/ru-vit` contains the manifest, briefings, mission tables,
  weapon descriptions, all three font sheets and the localized map/title data.
- Verify all four `assets/dossiers/screens/dossier_*.png` pages are present.
- Verify the archive contains no `syphon_filter_cheats`, save, BIN, CUE, CMD,
  log, dump, PDB, LIB or EXP file.
- Test only by manually extracting to a clean folder; never execute from `dist`.

## 5. Publish

- Commit the source and documentation.
- Create an annotated tag named `v<version>`.
- Push the default branch and tag.
- Create a GitHub Release from the tag.
- Upload both the ZIP and `.zip.sha256` sidecar.
- Put the supported disc revision, legal image requirement, archive SHA-256 and
  major changes in the release notes.

Release archives are intentionally not committed to Git history; GitHub Releases
is the canonical binary distribution channel.
