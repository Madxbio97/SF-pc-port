# Ghidra analysis

These headless scripts keep analysis exports deterministic and outside the source tree. Use Ghidra 12.1.2 with `ghidra_psx_ldr` 2026.07.08.

The analyzed EXE and Ghidra project belong under `out/` and are ignored by Git.

```powershell
$repoRoot = (Resolve-Path ../..).Path
$headless = Join-Path $env:GHIDRA_HOME 'support/analyzeHeadless.bat'
$project = Join-Path $repoRoot 'out/ghidra'
$scripts = Join-Path $repoRoot 'tools/ghidra'
$output = Join-Path $repoRoot 'out'

& $headless $project SyphonFilter -process SCUS_942.40 `
  -noanalysis -scriptPath $scripts `
  -postScript ApplyKnownSymbols.java `
  -postScript ExportFunctions.java (Join-Path $output 'ghidra-functions.csv') `
  -postScript ExportDecomp.java 0x8001457c (Join-Path $output 'main.c')
```

`0x8001457c` is the game `main` reached by the PS-X runtime entry at `0x800e3da4`.

`ApplyKnownSymbols.java` contains only addresses verified from control flow and decompilation. Inferred names are descriptive and do not claim to be the original identifiers.

Import the title overlay after extracting it with `sf_tool`:

```powershell
& $headless $project SyphonFilter -process SCUS_942.40 `
  -noanalysis -scriptPath $scripts `
  -postScript ImportOverlay.java TITLE_OVL (Join-Path $output 'TITLE.OVL') `
    0x80146630 0x80149cf4 Title_Update `
  -postScript ExportDecomp.java TITLE_OVL::80149cf4 `
    (Join-Path $output 'title_update.c')
```

The script refuses to overwrite an existing overlay if its size, base, or bytes differ.

Apply the recovered descriptive title symbols before exporting decompilation:

```powershell
& $headless $project SyphonFilter -process SCUS_942.40 `
  -noanalysis -scriptPath $scripts `
  -postScript ApplyTitleSymbols.java
```

Export all analyzed functions from the imported overlay with:

```powershell
& $headless $project SyphonFilter -process SCUS_942.40 `
  -noanalysis -scriptPath $scripts `
  -postScript ExportOverlayDecomp.java TITLE_OVL `
    (Join-Path $output 'title_overlay.c')
```

For a data or function address whose callers are still unknown, export stable
cross-reference locations and containing functions with:

```powershell
& $headless $project SyphonFilter -process SCUS_942.40 `
  -noanalysis -scriptPath $scripts `
  -postScript ExportReferences.java 0x8012c7b0 `
    (Join-Path $output 'references.txt')
```
