param(
    [string]$Version = "0.1.0-public-test.6",
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot "build\windows-psycross\$Configuration"
$distDir = Join-Path $repoRoot "dist"
$packageName = "SyphonFilterPC-$Version-win64"
$packageDir = Join-Path $distDir $packageName
$archivePath = Join-Path $distDir "$packageName.zip"
$archiveHashPath = "$archivePath.sha256"

foreach ($path in @($packageDir, $archivePath, $archiveHashPath)) {
    if (Test-Path -LiteralPath $path) {
        throw "Refusing to overwrite an existing release artifact: $path"
    }
}

$runtimeFiles = @(
    "syphon_filter.exe",
    "avcodec-62.dll",
    "avformat-62.dll",
    "avutil-60.dll",
    "fmt.dll",
    "OpenAL32.dll",
    "SDL2.dll",
    "swresample-6.dll",
    "swscale-9.dll"
)

foreach ($file in $runtimeFiles) {
    $source = Join-Path $buildDir $file
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Required runtime file is missing: $source"
    }
}

$vcRedistRoots = [Collections.Generic.List[string]]::new()
if ($env:VCToolsRedistDir) {
    $vcRedistRoots.Add($env:VCToolsRedistDir)
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
    $installations = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Redist.14.Latest -property installationPath
    foreach ($installation in $installations) {
        if ($installation) {
            $vcRedistRoots.Add((Join-Path $installation "VC\Redist\MSVC"))
        }
    }
}

$vcRuntimeDir = $vcRedistRoots |
    Where-Object { Test-Path -LiteralPath $_ -PathType Container } |
    ForEach-Object {
        Get-ChildItem -LiteralPath $_ -Recurse -Filter "vcruntime140.dll" -File -ErrorAction SilentlyContinue
    } |
    Where-Object { $_.FullName -match "\\x64\\Microsoft\.VC\d+\.CRT\\vcruntime140\.dll$" -and $_.FullName -notmatch "\\onecore\\" } |
    Sort-Object FullName -Descending -Unique |
    Select-Object -First 1 -ExpandProperty DirectoryName

if (-not $vcRuntimeDir) {
    throw "Microsoft Visual C++ x64 runtime was not found. Install the VC++ workload or set VCToolsRedistDir."
}

$vcFiles = @(
    "msvcp140.dll",
    "msvcp140_2.dll",
    "msvcp140_atomic_wait.dll",
    "vcruntime140.dll",
    "vcruntime140_1.dll"
)

New-Item -ItemType Directory -Path $packageDir | Out-Null
New-Item -ItemType Directory -Path (Join-Path $packageDir "licenses") | Out-Null

foreach ($file in $runtimeFiles) {
    Copy-Item -LiteralPath (Join-Path $buildDir $file) -Destination $packageDir
}

$dossierSource = Join-Path $buildDir "assets\dossiers\screens"
$dossierDestination = Join-Path $packageDir "assets\dossiers\screens"
New-Item -ItemType Directory -Path $dossierDestination | Out-Null
foreach ($file in @("dossier_01.png", "dossier_02.png", "dossier_03.png", "dossier_04.png")) {
    $source = Join-Path $dossierSource $file
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Required dossier image is missing: $source"
    }
    Copy-Item -LiteralPath $source -Destination $dossierDestination
}

foreach ($file in $vcFiles) {
    $source = Join-Path $vcRuntimeDir $file
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Required Visual C++ runtime file is missing: $source"
    }
    Copy-Item -LiteralPath $source -Destination $packageDir
}

Copy-Item -LiteralPath (Join-Path $repoRoot "LICENSE") -Destination (Join-Path $packageDir "LICENSE.txt")
Copy-Item -LiteralPath (Join-Path $repoRoot "external\PsyCross\LICENSE") -Destination (Join-Path $packageDir "licenses\PsyCross.txt")

$vcpkgShare = Join-Path $repoRoot "build\windows-psycross\vcpkg_installed\x64-windows\share"
$licenseSources = @{
    "FFmpeg.txt" = Join-Path $vcpkgShare "ffmpeg\copyright"
    "fmt.txt" = Join-Path $vcpkgShare "fmt\copyright"
    "OpenAL-Soft.txt" = Join-Path $vcpkgShare "openal-soft\copyright"
    "SDL2.txt" = Join-Path $vcpkgShare "sdl2\copyright"
}

foreach ($entry in $licenseSources.GetEnumerator()) {
    if (-not (Test-Path -LiteralPath $entry.Value -PathType Leaf)) {
        throw "Required license file is missing: $($entry.Value)"
    }
    Copy-Item -LiteralPath $entry.Value -Destination (Join-Path $packageDir "licenses\$($entry.Key)")
}

$readme = @"
SYPHON FILTER PC — PUBLIC TEST 0.1.0-PT6
Windows x64, 24 July 2026

ВАЖНО
======
Пакет не содержит игру или образ диска. Нужна ваша легальная копия
Syphon Filter USA v1.1: SCUS-94240, BIN/CUE.

ЗАПУСК
======
1. Распакуйте ZIP в отдельную папку.
2. Запустите syphon_filter.exe.
3. Нажмите BROWSE и выберите CUE-файл образа игры.
4. Настройте графику и управление, затем нажмите DEPLOY.

BIN-файлы должны оставаться рядом с CUE-файлом согласно его содержимому.
Путь к образу запоминается лаунчером. CMD-скрипт не требуется.

ЧИСТЫЙ СТАРТ
=============
В пакете нет сохранений, настроек, образов игры и файла syphon_filter_cheats.
Выбор миссий открывается только игровым прогрессом. Developer-функции доступны
только если пользователь самостоятельно создаст файл syphon_filter_cheats рядом с EXE.

СИСТЕМНЫЕ ТРЕБОВАНИЯ
====================
- Windows 10/11 x64.
- Видеокарта и драйвер с OpenGL 3.x.
- Syphon Filter USA v1.1 (SCUS-94240), BIN/CUE.
- Клавиатура/мышь или совместимый геймпад.

СОХРАНЕНИЯ И НАСТРОЙКИ
======================
Пользовательские сохранения и настройки хранятся в:
%LOCALAPPDATA%\SyphonFilterPC

ОТЧЁТ ОБ ОШИБКЕ
================
Укажите номер миссии, шаги воспроизведения, модель видеокарты и приложите
скриншот и созданный рядом с EXE файл *.log. Не отправляйте BIN/CUE-файлы.

This is an unofficial work-in-progress compatibility runtime. It is not
affiliated with or endorsed by Sony Interactive Entertainment.
"@

$notes = @"
PUBLIC TEST 0.1.0-PT6
=====================

- Встроенный лаунчер с выбором CUE, иконкой и единым оформлением окон.
- В лаунчер добавлен бонусный архив DOSSIERS с четырьмя страницами материалов.
- Иконка лаунчера заменена на портрет Гейба и содержит полный DPI-набор.
- Страницы досье получили лёгкое повышение резкости для читаемости текста.
- Настройки внутреннего рендера применяются ко всей сцене; добавлены бесшовная
  билинейная и анизотропная фильтрация.
- Улучшено FMV-видео и устранён лишний дизеринг.
- Переработано игровое меню: карта, цели, оружие, параметры и звук паузы.
- Исправлено сохранение прогресса и последовательное открытие пройденных миссий.
- Исправлены вспышки оружия игрока и противников, предметы, частицы и гранаты.
- Исправлены исчезающие модели, разрушаемые объекты и их состояние после рестарта.
- Восстановлено разрушение окон, стеклянных панелей и витражей на осколки.
- Исправлено восстановление разрушаемых объектов после провала миссии.
- Убран отладочный счётчик FPS из игрового изображения.
- Исправлены освещение оружейных ящиков и ряд ошибок текстур/моделей уровней.
- Устранён вылет renderer/UI bridge при потоковой загрузке секций складов PHARCOM.

Архив не содержит образ игры, сохранения, настройки или syphon_filter_cheats.
"@

$commit = try { (& git -C $repoRoot rev-parse --short HEAD 2>$null).Trim() } catch { "unknown" }
if (-not $commit) { $commit = "unknown" }

$buildInfo = @"
Product: Syphon Filter PC
Channel: Public Test
Version: $Version
Platform: Windows x64
Build type: $Configuration
Build date: 2026-07-24
Source revision: $commit
Supported disc: Syphon Filter USA v1.1, SCUS-94240, BIN/CUE
Launcher: integrated; no CMD bootstrap
Game image included: no
Save data included: no
Cheat marker included: no
"@

$notices = @"
This package includes or dynamically links third-party software.

FFmpeg — LGPL/GPL components as built by the package toolchain.
SDL2 — zlib license.
OpenAL Soft — LGPL license.
fmt — MIT license.
PsyCross — MIT license.
Microsoft Visual C++ Runtime — redistributed under the applicable Microsoft
Visual Studio license terms.

See the licenses directory for the supplied license texts.
"@

$utf8 = New-Object System.Text.UTF8Encoding($true)
[IO.File]::WriteAllText((Join-Path $packageDir "README_FIRST.txt"), $readme, $utf8)
[IO.File]::WriteAllText((Join-Path $packageDir "PUBLIC_TEST_NOTES.txt"), $notes, $utf8)
[IO.File]::WriteAllText((Join-Path $packageDir "BUILD_INFO.txt"), $buildInfo, $utf8)
[IO.File]::WriteAllText((Join-Path $packageDir "THIRD_PARTY_NOTICES.txt"), $notices, $utf8)

$forbidden = Get-ChildItem -LiteralPath $packageDir -Recurse -File | Where-Object {
    $_.Name -match "(?i)(syphon_filter_cheats|save|\.cue$|\.bin$|\.cmd$|\.log$|\.pdb$|\.lib$|\.exp$)"
}
if ($forbidden) {
    throw "Forbidden files found in release: $($forbidden.FullName -join ', ')"
}

$hashLines = Get-ChildItem -LiteralPath $packageDir -Recurse -File |
    Where-Object { $_.Name -ne "SHA256SUMS.txt" } |
    Sort-Object FullName |
    ForEach-Object {
        $relative = $_.FullName.Substring($packageDir.Length + 1).Replace("\", "/")
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash.ToLowerInvariant()
        "$hash  $relative"
    }
[IO.File]::WriteAllLines((Join-Path $packageDir "SHA256SUMS.txt"), $hashLines, $utf8)

Compress-Archive -LiteralPath $packageDir -DestinationPath $archivePath -CompressionLevel Optimal
$archiveHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archivePath).Hash.ToLowerInvariant()
[IO.File]::WriteAllText($archiveHashPath, "$archiveHash  $packageName.zip`r`n", $utf8)

Write-Output $packageDir
Write-Output $archivePath
Write-Output $archiveHashPath
