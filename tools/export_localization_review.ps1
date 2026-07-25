[CmdletBinding()]
param(
  [string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot),
  [string]$OutputBase = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($OutputBase)) {
  $OutputBase = Join-Path $RepositoryRoot "docs\RUSSIAN_LOCALIZATION_REVIEW"
}

$script:rows = [System.Collections.Generic.List[object]]::new()

function Add-ReviewRow {
  param(
    [string]$Category,
    [string]$Context,
    [string]$Source,
    [string]$Russian,
    [string]$Origin
  )
  $script:rows.Add([pscustomobject]@{
      Id = "SF-RU-{0:D4}" -f ($script:rows.Count + 1)
      Category = $Category
      Context = $Context
      Source = $Source
      Russian = $Russian
      Origin = $Origin
    })
}

function ConvertFrom-CppStringGroup {
  param([string]$Value)
  $result = [System.Text.StringBuilder]::new()
  $literals = [regex]::Matches(
    $Value,
    '(?:u8)?"(?<body>(?:\\.|[^"\\])*)"',
    [System.Text.RegularExpressions.RegexOptions]::Singleline)
  foreach ($literal in $literals) {
    $body = $literal.Groups['body'].Value
    for ($index = 0; $index -lt $body.Length; ++$index) {
      $character = $body[$index]
      if ($character -ne '\') {
        [void]$result.Append($character)
        continue
      }
      ++$index
      if ($index -ge $body.Length) {
        throw "Unterminated C++ escape in localization table"
      }
      $escape = $body[$index]
      switch ($escape) {
        'n' { [void]$result.Append("`n") }
        'r' { [void]$result.Append("`r") }
        't' { [void]$result.Append("`t") }
        '\' { [void]$result.Append('\') }
        '"' { [void]$result.Append('"') }
        "'" { [void]$result.Append("'") }
        default {
          if ($escape -ge '0' -and $escape -le '7') {
            $digits = [string]$escape
            for ($extra = 0; $extra -lt 2 -and $index + 1 -lt $body.Length; ++$extra) {
              $next = $body[$index + 1]
              if ($next -lt '0' -or $next -gt '7') {
                break
              }
              ++$index
              $digits += $next
            }
            [void]$result.Append([char][Convert]::ToInt32($digits, 8))
          } else {
            [void]$result.Append($escape)
          }
        }
      }
    }
  }
  return $result.ToString()
}

$script:vitMap = @{}
$lower = @(0x0410, 0x0412, 0x0421, 0x041E, 0x0415, 0x0401, 0x042A,
           0x041D, 0x0418, 0x042B, 0x041A, 0x0427, 0x041C, 0x041F,
           0x041E, 0x0420, 0x042F, 0x042C, 0x041B, 0x0422, 0x0413,
           0x0424, 0x0414, 0x0425, 0x0423, 0x0417)
for ($index = 0; $index -lt $lower.Count; ++$index) {
  $script:vitMap[0x61 + $index] = [char]$lower[$index]
}
$extended = @(0x042E, 0x0428, 0x0429, 0x0411, 0x0411, 0x0416, 0x0419,
              0x0419, 0x0416, 0x0417, 0x0401, 0x044E, 0x0426, 0x0429,
              0x0423, 0x042A, 0x0417, 0x0414, 0x0413, 0x0424, 0x042D,
              0x041B, 0x0418, 0x0419, 0x0428, 0x0427, 0x042F, 0x042C,
              0x041F, 0x0426)
for ($index = 0; $index -lt $extended.Count; ++$index) {
  $script:vitMap[0xDF + $index] = [char]$extended[$index]
}
$script:vitUpperMap = @{
  0x41 = [char]0x0410 # A -> А
  0x42 = [char]0x0412 # B -> В
  0x43 = [char]0x0421 # C -> С
  0x45 = [char]0x0415 # E -> Е
  0x48 = [char]0x041D # H -> Н
  0x4B = [char]0x041A # K -> К
  0x4D = [char]0x041C # M -> М
  0x4F = [char]0x041E # O -> О
  0x50 = [char]0x0420 # P -> Р
  0x54 = [char]0x0422 # T -> Т
  0x58 = [char]0x0425 # X -> Х
  0x59 = [char]0x0423 # Y -> У
  0x5A = [char]0x0417 # Z -> З
}

function ConvertFrom-VitBytes {
  param([byte[]]$Bytes)
  $result = [System.Text.StringBuilder]::new()
  foreach ($value in $Bytes) {
    if ($script:vitMap.ContainsKey([int]$value)) {
      [void]$result.Append($script:vitMap[[int]$value])
    } elseif ($script:vitUpperMap.ContainsKey([int]$value)) {
      [void]$result.Append($script:vitUpperMap[[int]$value])
    } else {
      [void]$result.Append([char]$value)
    }
  }
  # The ViT font deliberately reuses Latin glyph slots for Cyrillic letters.
  # Restore the few genuine Latin technical tokens after making the review text Unicode-clean.
  return $result.ToString().Replace('СВDС', 'CBDC').Replace('SТАRТ', 'START').Replace('МRВF', 'MRBF').Replace('FЕМА', 'FEMA').Replace('С4', 'C4')
}

function ConvertFrom-VitString {
  param([string]$Value)
  $bytes = [byte[]]::new($Value.Length)
  for ($index = 0; $index -lt $Value.Length; ++$index) {
    $bytes[$index] = [byte][int]$Value[$index]
  }
  return ConvertFrom-VitBytes $bytes
}

function Read-PackString {
  param([System.IO.BinaryReader]$Reader)
  $length = $Reader.ReadUInt32()
  if ($length -gt 1048576) {
    throw "Localization string is unreasonably large: $length"
  }
  return $Reader.ReadBytes([int]$length)
}

function Escape-ReviewCell {
  param([AllowEmptyString()][string]$Value)
  return $Value.Replace('\', '\\').Replace("`r", '\r').Replace("`n", '\n').Replace("`t", '\t')
}

$localizationSource = Join-Path $RepositoryRoot "src\game\localization.cpp"
$code = [System.IO.File]::ReadAllText($localizationSource, [System.Text.Encoding]::UTF8)
$baseStart = $code.IndexOf('constexpr std::array base_utf8_translations{')
$utf8Start = $code.IndexOf('constexpr std::array utf8_translations{', $baseStart)
$briefingStart = $code.IndexOf('constexpr std::array localized_briefings{', $utf8Start)
$briefingEnd = $code.IndexOf('std::optional<char> vitByteForCyrillic', $briefingStart)
if ($baseStart -lt 0 -or $utf8Start -lt 0 -or $briefingStart -lt 0 -or $briefingEnd -lt 0) {
  throw "Could not locate localization tables in $localizationSource"
}

$effective = [System.Collections.Generic.Dictionary[string, object]]::new([System.StringComparer]::Ordinal)
$order = [System.Collections.Generic.List[string]]::new()
$utf8Pattern = 'Utf8Translation\{\s*(?<source>(?:"(?:\\.|[^"\\])*"\s*)+),\s*(?<target>(?:u8"(?:\\.|[^"\\])*"\s*)+),?\s*\}'
foreach ($table in @(
    [pscustomobject]@{ Start = $baseStart; End = $utf8Start; Origin = 'proofread common catalogue' },
    [pscustomobject]@{ Start = $utf8Start; End = $briefingStart; Origin = 'proofread gameplay catalogue' }
  )) {
  $section = $code.Substring($table.Start, $table.End - $table.Start)
  foreach ($match in [regex]::Matches($section, $utf8Pattern, [System.Text.RegularExpressions.RegexOptions]::Singleline)) {
    $source = ConvertFrom-CppStringGroup $match.Groups['source'].Value
    $target = ConvertFrom-CppStringGroup $match.Groups['target'].Value
    if (-not $effective.ContainsKey($source)) {
      $order.Add($source)
    }
    $effective[$source] = [pscustomobject]@{ Russian = $target; Origin = $table.Origin }
  }
}

foreach ($source in $order) {
  $entry = $effective[$source]
  Add-ReviewRow 'STATIC_UI_GAMEPLAY' 'menus, HUD, prompts and notifications' $source $entry.Russian $entry.Origin
}

Add-ReviewRow 'DYNAMIC_GAMEPLAY' 'item pickup template' '%s taken' 'ПОЛУЧЕНО: %s' 'native composition rule'
Add-ReviewRow 'DYNAMIC_GAMEPLAY' 'ammunition pickup template' '%s bullet(s)/shell(s) taken' 'ПАТРОНЫ: %s' 'native composition rule'
Add-ReviewRow 'DYNAMIC_GAMEPLAY' 'full inventory notification' '%s%ss maxed' 'ЗАПАС ПОЛОН' 'native composition rule'
Add-ReviewRow 'DYNAMIC_GAMEPLAY' 'generic elimination objective' 'Eliminate %s' 'ЛИКВИДИРОВАТЬ ЦЕЛЬ' 'native composition rule'
Add-ReviewRow 'DYNAMIC_GAMEPLAY' 'generic security-item objective' 'Find security %s%s' 'НАЙТИ КЛЮЧ-КАРТУ ОХРАНЫ' 'native composition rule'

$missionPack = Join-Path $RepositoryRoot 'assets\locales\ru-vit\mission_menu.dat'
$stream = [System.IO.File]::OpenRead($missionPack)
$reader = [System.IO.BinaryReader]::new($stream)
try {
  $magic = [System.Text.Encoding]::ASCII.GetString($reader.ReadBytes(8)).TrimEnd([char]0)
  if ($magic -ne 'SFLMNU2') {
    throw "Unexpected mission-menu magic: $magic"
  }
  $missionCount = $reader.ReadUInt32()
  for ($mission = 0; $mission -lt $missionCount; ++$mission) {
    $entryCount = $reader.ReadUInt32()
    for ($entryIndex = 0; $entryIndex -lt $entryCount; ++$entryIndex) {
      $source = [System.Text.Encoding]::ASCII.GetString((Read-PackString $reader))
      $packRussian = ConvertFrom-VitBytes (Read-PackString $reader)
      if ($effective.ContainsKey($source)) {
        $russian = $effective[$source].Russian
        $origin = 'proofread native override (replaces mission_menu.dat)'
      } else {
        $russian = $packRussian
        $origin = 'mission_menu.dat'
      }
      Add-ReviewRow 'MISSION_TEXT' ("mission {0:D2}, entry {1:D2}" -f ($mission + 1), ($entryIndex + 1)) $source $russian $origin
    }
  }
} finally {
  $reader.Dispose()
  $stream.Dispose()
}

$briefingSection = $code.Substring($briefingStart, $briefingEnd - $briefingStart)
$briefingPattern = 'Utf8MissionBriefing\{\s*(?<location>(?:u8"(?:\\.|[^"\\])*"\s*)+),\s*(?<title>(?:"(?:\\.|[^"\\])*"\s*)+),\s*(?<date>(?:"(?:\\.|[^"\\])*"\s*)+),\s*(?<directive>(?:u8"(?:\\.|[^"\\])*"\s*)+),\s*(?<additional>(?:u8"(?:\\.|[^"\\])*"\s*)+),?\s*\}'
$mission = 0
foreach ($match in [regex]::Matches($briefingSection, $briefingPattern, [System.Text.RegularExpressions.RegexOptions]::Singleline)) {
  ++$mission
  $titleSource = ConvertFrom-CppStringGroup $match.Groups['title'].Value
  $title = if ($effective.ContainsKey($titleSource)) { $effective[$titleSource].Russian } else { $titleSource }
  $values = @(
    (ConvertFrom-CppStringGroup $match.Groups['location'].Value),
    $title,
    (ConvertFrom-CppStringGroup $match.Groups['date'].Value),
    (ConvertFrom-CppStringGroup $match.Groups['directive'].Value),
    (ConvertFrom-CppStringGroup $match.Groups['additional'].Value)
  )
  $fields = @('location', 'mission title', 'date/time', 'directive', 'additional directive')
  for ($field = 0; $field -lt $fields.Count; ++$field) {
    Add-ReviewRow 'BRIEFING' ("mission {0:D2}, {1}" -f $mission, $fields[$field]) '' $values[$field] 'proofread built-in briefing'
  }
}

# Weapon descriptions now come from the proofread UTF-8 catalogue above.
# The historical WEAPDESC.TXT remains import-compatible but is deliberately
# excluded because it is no longer an effective runtime text source.

$graphicRoot = Join-Path $RepositoryRoot 'assets\locales\ru-vit'
$repositoryUri = [Uri]((Resolve-Path $RepositoryRoot).Path.TrimEnd('\') + '\')
Get-ChildItem -Path (Join-Path $graphicRoot 'title'), (Join-Path $graphicRoot 'maps') -Filter '*.TIM' -File -Recurse |
  Sort-Object FullName |
  ForEach-Object {
    $relative = [Uri]::UnescapeDataString(
      $repositoryUri.MakeRelativeUri([Uri]$_.FullName).ToString()).Replace('/', '\')
    Add-ReviewRow 'GRAPHIC_TEXT_ASSET' $relative '' '[текст встроен в изображение TIM; проверяется визуально]' 'binary image asset'
  }

$outputDirectory = Split-Path -Parent $OutputBase
[System.IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
$utf8Bom = [System.Text.UTF8Encoding]::new($true)

$tsv = [System.Text.StringBuilder]::new()
[void]$tsv.AppendLine("ID`tCATEGORY`tCONTEXT`tENGLISH_SOURCE`tRUSSIAN_EFFECTIVE`tORIGIN")
foreach ($row in $rows) {
  [void]$tsv.Append((Escape-ReviewCell $row.Id)).Append("`t")
  [void]$tsv.Append((Escape-ReviewCell $row.Category)).Append("`t")
  [void]$tsv.Append((Escape-ReviewCell $row.Context)).Append("`t")
  [void]$tsv.Append((Escape-ReviewCell $row.Source)).Append("`t")
  [void]$tsv.Append((Escape-ReviewCell $row.Russian)).Append("`t")
  [void]$tsv.AppendLine((Escape-ReviewCell $row.Origin))
}
[System.IO.File]::WriteAllText("$OutputBase.tsv", $tsv.ToString(), $utf8Bom)

$review = [System.Text.StringBuilder]::new()
[void]$review.AppendLine('SYPHON FILTER PC — ПОЛНЫЙ РЕЕСТР РУССКОЙ ЛОКАЛИЗАЦИИ')
[void]$review.AppendLine('Формат замечания: ID -> предложенный русский текст / комментарий.')
[void]$review.AppendLine('Последовательности \n и \t обозначают перенос строки и табуляцию.')
[void]$review.AppendLine()
foreach ($row in $rows) {
  [void]$review.AppendLine("[$($row.Id)] $($row.Category) — $($row.Context)")
  if (-not [string]::IsNullOrEmpty($row.Source)) {
    [void]$review.AppendLine("EN: " + (Escape-ReviewCell $row.Source))
  }
  [void]$review.AppendLine("RU: " + (Escape-ReviewCell $row.Russian))
  [void]$review.AppendLine("SOURCE: $($row.Origin)")
  [void]$review.AppendLine('COMMENT:')
  [void]$review.AppendLine()
}
[System.IO.File]::WriteAllText("$OutputBase.txt", $review.ToString(), $utf8Bom)

Write-Output ("Exported {0} entries" -f $rows.Count)
Write-Output "$OutputBase.txt"
Write-Output "$OutputBase.tsv"
