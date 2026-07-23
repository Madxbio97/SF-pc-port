param(
    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

$size = 32
$maskStride = [int][Math]::Floor(($size + 31) / 32) * 4
$bitmapBytes = 40 + ($size * $size * 4) + ($maskStride * $size)
$directory = [System.IO.Path]::GetDirectoryName($OutputPath)
if ($directory) {
    [System.IO.Directory]::CreateDirectory($directory) | Out-Null
}

$stream = [System.IO.File]::Create($OutputPath)
$writer = [System.IO.BinaryWriter]::new($stream)
try {
    $writer.Write([uint16]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]1)
    $writer.Write([byte]$size)
    $writer.Write([byte]$size)
    $writer.Write([byte]0)
    $writer.Write([byte]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]32)
    $writer.Write([uint32]$bitmapBytes)
    $writer.Write([uint32]22)

    $writer.Write([uint32]40)
    $writer.Write([int32]$size)
    $writer.Write([int32]($size * 2))
    $writer.Write([uint16]1)
    $writer.Write([uint16]32)
    $writer.Write([uint32]0)
    $writer.Write([uint32]($size * $size * 4))
    $writer.Write([int32]0)
    $writer.Write([int32]0)
    $writer.Write([uint32]0)
    $writer.Write([uint32]0)

    for ($y = $size - 1; $y -ge 0; --$y) {
        for ($x = 0; $x -lt $size; ++$x) {
            $border = $x -lt 2 -or $y -lt 2 -or $x -ge ($size - 2) -or $y -ge ($size - 2)
            $diagonal = [Math]::Abs($x - $y) -le 2 -or [Math]::Abs(($size - 1 - $x) - $y) -le 2
            $center = [Math]::Abs($x - ($size / 2)) -le 2 -or [Math]::Abs($y - ($size / 2)) -le 2
            if ($center -and $diagonal) {
                $red, $green, $blue = 242, 154, 46
            } elseif ($border) {
                $red, $green, $blue = 80, 103, 196
            } else {
                $red, $green, $blue = 7, 13, 29
            }
            $writer.Write([byte]$blue)
            $writer.Write([byte]$green)
            $writer.Write([byte]$red)
            $writer.Write([byte]255)
        }
    }
    for ($index = 0; $index -lt ($maskStride * $size); ++$index) {
        $writer.Write([byte]0)
    }
} finally {
    $writer.Dispose()
    $stream.Dispose()
}
