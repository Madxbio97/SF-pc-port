#!/usr/bin/env python3
"""Build the compact 4-bpp PS1 muzzle-flash texture header from a PNG."""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageChops


WIDTH = 48
HEIGHT = 48


def psx_color(red: int, green: int, blue: int) -> int:
    return 0x8000 | (red >> 3) | ((green >> 3) << 5) | ((blue >> 3) << 10)


def format_words(words: list[int], per_line: int = 8) -> str:
    lines = []
    for offset in range(0, len(words), per_line):
        values = ", ".join(f"0x{word:04x}U" for word in words[offset : offset + per_line])
        lines.append(f"    {values},")
    return "\n".join(lines)


def build(source_path: Path, preview_path: Path, header_path: Path) -> None:
    source = Image.open(source_path).convert("RGB")
    red, green, blue = source.split()
    brightness = ImageChops.lighter(red, ImageChops.lighter(green, blue))
    mask = brightness.point(lambda value: 255 if value > 8 else 0)
    bounds = mask.getbbox()
    if bounds is None:
        raise ValueError("source image contains no visible muzzle flash")

    left, top, right, bottom = bounds
    side = max(right - left, bottom - top)
    margin = max(4, round(side * 0.12))
    side += margin * 2
    centre_x = (left + right) // 2
    centre_y = (top + bottom) // 2
    crop = source.crop(
        (
            centre_x - side // 2,
            centre_y - side // 2,
            centre_x - side // 2 + side,
            centre_y - side // 2 + side,
        )
    )
    sprite = crop.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)
    sprite.putdata(
        [(0, 0, 0) if max(pixel) <= 8 else pixel for pixel in sprite.getdata()]
    )

    quantized = sprite.quantize(
        colors=16,
        method=Image.Quantize.MEDIANCUT,
        dither=Image.Dither.FLOYDSTEINBERG,
    )
    source_palette = quantized.getpalette()[: 16 * 3]
    colors = [tuple(source_palette[index : index + 3]) for index in range(0, 48, 3)]
    transparent = min(range(16), key=lambda index: sum(colors[index]))
    order = [transparent, *(index for index in range(16) if index != transparent)]
    remap = {old: new for new, old in enumerate(order)}
    palette = [colors[index] for index in order]
    palette[0] = (0, 0, 0)

    indexed = Image.new("P", (WIDTH, HEIGHT))
    indices = [remap[index] for index in quantized.getdata()]
    indexed.putdata(indices)
    flat_palette = [channel for color in palette for channel in color]
    indexed.putpalette(flat_palette + [0] * (768 - len(flat_palette)))
    preview_path.parent.mkdir(parents=True, exist_ok=True)
    indexed.save(preview_path)

    packed = []
    for row in range(HEIGHT):
        for column in range(0, WIDTH, 4):
            word = 0
            for nibble in range(4):
                word |= indices[row * WIDTH + column + nibble] << (nibble * 4)
            packed.append(word)
    clut = [0, *(psx_color(*color) for color in palette[1:])]
    header = f"""#pragma once

#include <array>
#include <cstdint>

namespace sf::platform::detail::muzzle_flash_texture {{

inline constexpr std::uint16_t width = {WIDTH}U;
inline constexpr std::uint16_t height = {HEIGHT}U;
inline constexpr std::uint16_t width_words = width / 4U;

inline constexpr std::array<std::uint16_t, {len(packed)}U> pixels{{{{
{format_words(packed)}
}}}};

inline constexpr std::array<std::uint16_t, 16U> clut{{{{
{format_words(clut, 4)}
}}}};

}} // namespace sf::platform::detail::muzzle_flash_texture
"""
    header_path.parent.mkdir(parents=True, exist_ok=True)
    header_path.write_text(header, encoding="utf-8", newline="\n")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("preview", type=Path)
    parser.add_argument("header", type=Path)
    arguments = parser.parse_args()
    build(arguments.source, arguments.preview, arguments.header)


if __name__ == "__main__":
    main()
