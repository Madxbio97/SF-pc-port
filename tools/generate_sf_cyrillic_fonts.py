#!/usr/bin/env python3
"""Generate and preview the Russian PS1 font sheets used by the PC port.

The game addresses one logical 8-pixel-high font page split across three TIM
files and uses the ViT one-byte Cyrillic map.  This tool can still copy the
historical ViT sheets verbatim, but it can also rasterize one licensed
TrueType/OpenType font into every Latin, Cyrillic, digit and punctuation cell.
The generated sheets may use a denser 2x physical atlas while preserving the
retail logical UVs and advances used by the renderer.
"""

from __future__ import annotations

import argparse
import json
import struct
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


DIGITS = (
    (0, 0, 5), (8, 0, 5), (16, 0, 5), (24, 0, 5), (0, 8, 5),
    (8, 8, 5), (16, 8, 5), (24, 8, 5), (0, 16, 5), (8, 16, 5),
)
LOWER = (
    (8, 32, 5), (16, 32, 4), (24, 32, 4), (0, 40, 5),
    (8, 40, 4), (16, 40, 4), (24, 40, 5), (0, 48, 5),
    (8, 48, 6), (16, 48, 6), (24, 48, 4), (0, 56, 5),
    (8, 56, 6), (16, 56, 5), (24, 56, 5), (32, 0, 4),
    (40, 0, 5), (48, 0, 4), (56, 0, 5), (32, 8, 5),
    (40, 8, 4), (48, 8, 5), (56, 8, 6), (32, 16, 5),
    (40, 16, 4), (48, 16, 4),
)
UPPER = (
    (56, 16, 6), (32, 24, 5), (40, 24, 5), (48, 24, 6),
    (56, 24, 4), (32, 32, 4), (40, 32, 5), (48, 32, 5),
    (56, 32, 1), (32, 40, 4), (40, 40, 5), (48, 40, 4),
    (56, 40, 7), (32, 48, 6), (40, 48, 5), (48, 48, 5),
    (56, 48, 6), (32, 56, 5), (40, 56, 5), (48, 56, 5),
    (56, 56, 5), (64, 0, 6), (72, 0, 0), (88, 0, 6),
    (96, 0, 6), (64, 8, 5),
)
EXTENDED = (
    (64, 24, 6), (72, 24, 5), (80, 24, 6), (88, 24, 5),
    (96, 24, 4), (64, 32, 5), (72, 32, 5), (80, 32, 5),
    (88, 32, 5), (96, 32, 4), (64, 40, 4), (72, 40, 5),
    (80, 40, 5), (88, 40, 8), (96, 40, 4), (64, 48, 5),
    (72, 48, 5), (80, 48, 5), (88, 48, 3), (96, 48, 5),
    (64, 56, 4), (72, 56, 4), (80, 56, 5), (88, 56, 6),
    (96, 56, 7), (64, 64, 4), (72, 64, 4), (80, 64, 4),
    (88, 64, 4), (96, 64, 5),
)

GLYPH_CELLS = {
    **{ord("0") + index: cell for index, cell in enumerate(DIGITS)},
    **{ord("a") + index: cell for index, cell in enumerate(LOWER)},
    **{ord("A") + index: cell for index, cell in enumerate(UPPER)},
    **{0xDF + index: cell for index, cell in enumerate(EXTENDED)},
}

PUNCTUATION = {
    ord("?"): (0, 24, 4),
    ord("!"): (8, 24, 1),
    ord(":"): (16, 16, 2),
    ord("/"): (20, 16, 5),
    ord("."): (28, 16, 1),
    ord('"'): (24, 24, 3),
    ord("'"): (24, 24, 1),
    ord("("): (88, 16, 3),
    ord(")"): (96, 16, 3),
    ord(","): (0, 32, 1),
    ord("-"): (4, 32, 3),
}

# Bytes 0x61..0x7a are Cyrillic capitals in the ViT text encoding.  The
# duplicated О is intentional and matches the retail table exactly.
CYRILLIC_IN_LOWER_ASCII = (
    "А", "В", "С", "О", "Е", "Ё", "Ъ", "Н", "И", "Ы", "К", "Ч", "М",
    "П", "О", "Р", "Я", "Ь", "Л", "Т", "Г", "Ф", "Д", "Х", "У", "З",
)

CYRILLIC_EXTENDED = (
    "Ю", "Ш", "Щ", "Б", "Б", "Ж", "Й", "Й", "Ж", "З", "Ё", "ю", "Ц",
    "Щ", "У", "Ъ", "З", "Д", "Г", "Ф", "Э", "Л", "И", "Й", "Ш", "Ч",
    "Я", "Ь", "П", "Ц",
)

FONT_CHARACTERS = {
    **{ord("0") + index: str(index) for index in range(10)},
    **{ord("a") + index: value for index, value in enumerate(CYRILLIC_IN_LOWER_ASCII)},
    **{ord("A") + index: chr(ord("A") + index) for index in range(26)},
    **{0xDF + index: value for index, value in enumerate(CYRILLIC_EXTENDED)},
    **{code: chr(code) for code in PUNCTUATION},
}

# Neutral entries from the original CLUT, ordered from a faint antialias edge
# to the solid face color.  Four levels retain crisp PS1 pixels; using the
# entire 15-step retail ramp makes newly rasterized outlines look smeared.
FONT_RAMP = (187, 127, 74, 51)
TRANSPARENT_INDEX = 250

SHEET_LAYOUT = {
    "FONTA.TIM": (832, 0, 32, 64),
    "FONTB.TIM": (848, 0, 32, 64),
    "FONTC.TIM": (864, 0, 42, 123),
}

@dataclass
class TimSheet:
    path: Path
    data: bytearray
    clut: list[int]
    pixel_offset: int
    x: int
    y: int
    width: int
    height: int


def read_sheet(path: Path) -> TimSheet:
    data = bytearray(path.read_bytes())
    if struct.unpack_from("<I", data, 0)[0] != 0x10:
        raise ValueError(f"{path}: not a TIM image")
    clut_size, _, _, clut_width, clut_height = struct.unpack_from(
        "<IHHHH", data, 8
    )
    clut = [
        struct.unpack_from("<H", data, 20 + index * 2)[0]
        for index in range(clut_width * clut_height)
    ]
    block = 8 + clut_size
    _, x, y, width_words, height = struct.unpack_from("<IHHHH", data, block)
    return TimSheet(path, data, clut, block + 12, x, y, width_words * 2, height)


def load_atlas(source: Path) -> tuple[Image.Image, list[TimSheet]]:
    sheets = [read_sheet(source / name) for name in ("FONTA.TIM", "FONTB.TIM", "FONTC.TIM")]
    base_x = min(sheet.x for sheet in sheets)
    width = max((sheet.x - base_x) * 2 + sheet.width for sheet in sheets)
    height = max(sheet.y + sheet.height for sheet in sheets)
    atlas_size = 1
    while atlas_size < max(width, height, 128):
        atlas_size *= 2
    atlas = Image.new("L", (atlas_size, atlas_size), 0)
    for sheet in sheets:
        indices = sheet.data[
            sheet.pixel_offset : sheet.pixel_offset + sheet.width * sheet.height
        ]
        values = []
        for index in indices:
            color = sheet.clut[index] & 0x7FFF
            red = color & 31
            green = (color >> 5) & 31
            blue = (color >> 10) & 31
            values.append(round((red + green + blue) * 255 / (31 * 3)))
        image = Image.new("L", (sheet.width, sheet.height))
        image.putdata(values)
        atlas.paste(image, ((sheet.x - base_x) * 2, sheet.y))
    return atlas, sheets


def write_contact(
    atlas: Image.Image,
    output: Path,
    metrics: dict[int, int] | None = None,
    atlas_scale: int = 1,
) -> None:
    codes = list(range(ord("0"), ord("9") + 1))
    codes += list(range(ord("a"), ord("z") + 1))
    codes += list(range(ord("A"), ord("Z") + 1))
    codes += list(range(0xDF, 0xFD))
    contact = Image.new("RGB", (16 * 72, 6 * 88), "black")
    draw = ImageDraw.Draw(contact)
    for index, code in enumerate(codes):
        x, y, width = GLYPH_CELLS[code]
        if metrics is not None:
            width = metrics.get(code, width)
        width = max(width, 1)
        physical_width = width * atlas_scale
        glyph = atlas.crop(
            (
                x * atlas_scale,
                y * atlas_scale,
                x * atlas_scale + physical_width,
                y * atlas_scale + 8 * atlas_scale,
            )
        ).resize(
            (width * 8, 64), Image.Resampling.NEAREST
        )
        left = (index % 16) * 72
        top = (index // 16) * 88
        contact.paste(Image.merge("RGB", (glyph, glyph, glyph)), (left + 3, top + 3))
        draw.text((left + 3, top + 70), f"{code:02X}", fill="white")
    output.parent.mkdir(parents=True, exist_ok=True)
    contact.save(output)


def _sheet_for_pixel(sheets: list[TimSheet], x: int, y: int) -> tuple[TimSheet, int, int]:
    base_x = min(sheet.x for sheet in sheets)
    for sheet in sheets:
        left = (sheet.x - base_x) * 2
        if left <= x < left + sheet.width and 0 <= y < sheet.height:
            return sheet, x - left, y
    raise ValueError(f"font pixel ({x}, {y}) is outside the TIM sheets")


def _write_sheet_pixel(sheets: list[TimSheet], x: int, y: int, value: int) -> None:
    sheet, local_x, local_y = _sheet_for_pixel(sheets, x, y)
    sheet.data[sheet.pixel_offset + local_y * sheet.width + local_x] = value


def _read_sheet_pixel(sheets: list[TimSheet], x: int, y: int) -> int:
    sheet, local_x, local_y = _sheet_for_pixel(sheets, x, y)
    return sheet.data[sheet.pixel_offset + local_y * sheet.width + local_x]


def _quantize_coverage(value: int) -> int:
    if value < 40:
        return TRANSPARENT_INDEX
    # A mild gamma lift retains squared-off counters at only eight pixels.
    normalized = ((value - 40) / 215.0) ** 0.82
    ramp_index = min(len(FONT_RAMP) - 1, max(0, round(normalized * (len(FONT_RAMP) - 1))))
    return FONT_RAMP[ramp_index]


def _render_glyph(
    font: ImageFont.FreeTypeFont,
    character: str,
    maximum_width: int,
    height: int,
) -> Image.Image:
    left, top, right, bottom = font.getbbox(character, anchor="ls")
    width = max(1, right - left)
    canvas = Image.new("L", (width, height), 0)
    draw = ImageDraw.Draw(canvas)

    # Every glyph must use one shared baseline.  Do not fit each outline into
    # the cell independently: accented letters would be pushed down while
    # Д/Ц/Щ would be pulled up to make room for their descenders.  That was
    # the source of the visibly wavy Russian text.  The retail cell is allowed
    # to clip only the excess accent/descender pixels at its outer edges.
    baseline = height - 1
    draw.text((-left, baseline), character, font=font, fill=255, anchor="ls")

    if character in ("Ш", "Щ", "ш", "щ"):
        # Industry's middle stem begins at the x-height while the two outer
        # stems reach the cap height. At eight logical pixels this reads as a
        # visibly broken/short post. Find the first row containing all three
        # vertical strokes and continue only the middle run up to the common
        # top, preserving the font's own antialias coverage at both edges.
        top = next(
            (
                row
                for row in range(height)
                if any(
                    canvas.getpixel((column, row))
                    for column in range(canvas.width)
                )
            ),
            None,
        )
        if top is not None:
            for row in range(top, height):
                runs: list[tuple[int, int]] = []
                start = None
                for column in range(canvas.width + 1):
                    covered = (
                        column < canvas.width
                        and canvas.getpixel((column, row)) != 0
                    )
                    if covered and start is None:
                        start = column
                    elif not covered and start is not None:
                        runs.append((start, column))
                        start = None
                if len(runs) < 3:
                    continue
                middle_start, middle_end = runs[len(runs) // 2]
                for fill_row in range(top, row):
                    for column in range(middle_start, middle_end):
                        canvas.putpixel(
                            (column, fill_row), canvas.getpixel((column, row))
                        )
                break

    bounds = canvas.getbbox()
    if bounds is None:
        return Image.new("L", (1, height), 0)
    canvas = canvas.crop((bounds[0], 0, bounds[2], height))
    if canvas.width > maximum_width:
        canvas = canvas.resize((maximum_width, height), Image.Resampling.LANCZOS)
    return canvas


def _scaled_sheets(source: Path, atlas_scale: int) -> list[TimSheet]:
    templates = {
        sheet.path.name: sheet
        for sheet in (read_sheet(source / name) for name in SHEET_LAYOUT)
    }
    base_x = min(layout[0] for layout in SHEET_LAYOUT.values())
    result: list[TimSheet] = []
    for name, (x, y, width, height) in SHEET_LAYOUT.items():
        template = templates[name]
        scaled_x = base_x + (x - base_x) * atlas_scale
        scaled_y = y * atlas_scale
        scaled_width = width * atlas_scale
        scaled_height = height * atlas_scale
        if scaled_width % 2 != 0:
            raise ValueError(f"{name}: 8-bit TIM width must be even")
        width_words = scaled_width // 2
        pixel_bytes = scaled_width * scaled_height
        pixel_block = template.pixel_offset - 12
        data = bytearray(template.data[:pixel_block])
        data.extend(
            struct.pack(
                "<IHHHH",
                12 + pixel_bytes,
                scaled_x,
                scaled_y,
                width_words,
                scaled_height,
            )
        )
        data.extend(bytes([TRANSPARENT_INDEX]) * pixel_bytes)
        result.append(
            TimSheet(
                template.path,
                data,
                template.clut,
                pixel_block + 12,
                scaled_x,
                scaled_y,
                scaled_width,
                scaled_height,
            )
        )
    return result


def write_regenerated_fonts(
    source: Path,
    output: Path,
    font_path: Path,
    font_size: int,
    font_weight: int,
    atlas_scale: int,
) -> dict[int, int]:
    if not font_path.is_file():
        raise ValueError(f"font file does not exist: {font_path}")
    if font_size <= 0:
        raise ValueError("font size must be positive")
    if atlas_scale not in (1, 2):
        raise ValueError("atlas scale must be 1 or 2")
    sheets = _scaled_sheets(source, atlas_scale)
    font = ImageFont.truetype(str(font_path), font_size)
    axes = []
    if hasattr(font, "get_variation_axes"):
        try:
            axes = font.get_variation_axes()
        except OSError:
            # FreeType exposes the variation entry point for static TTFs too,
            # but reports an invalid-argument error when it is queried.
            axes = []
    if axes:
        if len(axes) != 1 or axes[0].get("name") != b"Weight":
            raise ValueError("only a single weight variation axis is supported")
        minimum = int(axes[0]["minimum"])
        maximum = int(axes[0]["maximum"])
        font.set_variation_by_axes([min(max(font_weight, minimum), maximum)])
    cells = {**GLYPH_CELLS, **PUNCTUATION}
    metrics: dict[int, int] = {}

    # Clear complete cells first.  Some punctuation shares a cell origin, so
    # deduplicate the rectangles before drawing the final glyphs.
    cleared: set[tuple[int, int]] = set()
    for x, y, _ in cells.values():
        if (x, y) in cleared:
            continue
        cleared.add((x, y))
        for row in range(8 * atlas_scale):
            for column in range(8 * atlas_scale):
                _write_sheet_pixel(
                    sheets,
                    x * atlas_scale + column,
                    y * atlas_scale + row,
                    TRANSPARENT_INDEX,
                )

    for code, character in FONT_CHARACTERS.items():
        x, y, historical_width = cells[code]
        # Preserve the retail advances so every existing menu/backdrop keeps
        # its authored layout.  ViT left Latin W empty; give that one repaired
        # cell the available eight pixels.
        fixed_width = historical_width or (8 if code == ord("W") else 1)
        maximum_width = fixed_width * atlas_scale
        glyph = _render_glyph(
            font,
            character,
            maximum_width,
            8 * atlas_scale,
        )
        metrics[code] = fixed_width
        # Centre narrow outlines inside their historical advance instead of
        # pinning every glyph to the left edge.  Industria has deliberately
        # narrow stems; balanced side bearings make menu lines look typeset
        # rather than assembled from unrelated bitmap cells.
        left_padding = max(0, (maximum_width - glyph.width) // 2)
        for row in range(8 * atlas_scale):
            for column in range(min(glyph.width, maximum_width)):
                _write_sheet_pixel(
                    sheets,
                    x * atlas_scale + left_padding + column,
                    y * atlas_scale + row,
                    _quantize_coverage(glyph.getpixel((column, row))),
                )

    def cell_pixels(code: int) -> bytes:
        x, y, _ = cells[code]
        return bytes(
            _read_sheet_pixel(
                sheets,
                x * atlas_scale + column,
                y * atlas_scale + row,
            )
            for row in range(8 * atlas_scale)
            for column in range(8 * atlas_scale)
        )

    def cell_bounds(code: int) -> tuple[int, int, int, int] | None:
        x, y, _ = cells[code]
        points = [
            (column, row)
            for row in range(8 * atlas_scale)
            for column in range(8 * atlas_scale)
            if _read_sheet_pixel(
                sheets,
                x * atlas_scale + column,
                y * atlas_scale + row,
            )
            != TRANSPARENT_INDEX
        ]
        if not points:
            return None
        return (
            min(point[0] for point in points),
            min(point[1] for point in points),
            max(point[0] for point in points),
            max(point[1] for point in points),
        )

    if cell_pixels(0xE5) == cell_pixels(0xF5):
        raise ValueError("Cyrillic Й and И produced identical atlas cells")
    breve_x, breve_y, _ = cells[0xE5]
    if not any(
        _read_sheet_pixel(
            sheets,
            breve_x * atlas_scale + column,
            breve_y * atlas_scale + row,
        )
        != TRANSPARENT_INDEX
        for row in range(2 * atlas_scale)
        for column in range(8 * atlas_scale)
    ):
        raise ValueError("Cyrillic Й lost its breve at the atlas boundary")

    reference_bounds = cell_bounds(ord("a"))  # ViT byte 0x61 is Cyrillic А.
    if reference_bounds is None:
        raise ValueError("Cyrillic А produced an empty atlas cell")
    reference_top = reference_bounds[1]
    reference_bottom = reference_bounds[3]
    for code in (ord("w"), 0xF0, 0xEB, 0xFC):  # Every encoded Д and Ц cell.
        bounds = cell_bounds(code)
        if bounds is None or bounds[1] != reference_top:
            raise ValueError(
                f"Cyrillic Д/Ц cell 0x{code:02X} does not share the font baseline"
            )
    for code in (0xE5, 0xE6, 0xF6):  # Every encoded Й cell.
        bounds = cell_bounds(code)
        if bounds is None or bounds[3] != reference_bottom:
            raise ValueError(
                f"Cyrillic Й cell 0x{code:02X} does not share the font baseline"
            )

    for code in (0xE0, 0xE1, 0xEC, 0xF7):  # Every encoded Ш/Щ cell.
        bounds = cell_bounds(code)
        if bounds is None:
            raise ValueError(f"Cyrillic Ш/Щ cell 0x{code:02X} is empty")
        x, y, _ = cells[code]
        runs: list[tuple[int, int]] = []
        start = None
        for column in range(8 * atlas_scale + 1):
            covered = (
                column < 8 * atlas_scale
                and _read_sheet_pixel(
                    sheets,
                    x * atlas_scale + column,
                    y * atlas_scale + bounds[1],
                )
                != TRANSPARENT_INDEX
            )
            if covered and start is None:
                start = column
            elif not covered and start is not None:
                runs.append((start, column))
                start = None
        if len(runs) < 3:
            raise ValueError(
                f"Cyrillic Ш/Щ cell 0x{code:02X} middle stem does not reach cap height"
            )

    output.mkdir(parents=True, exist_ok=True)
    for sheet in sheets:
        (output / sheet.path.name).write_bytes(sheet.data)
    return metrics


def write_metrics(
    metrics: dict[int, int], font_name: str, atlas_scale: int, output: Path
) -> None:
    payload = {
        "font": font_name,
        "logical_height": 8,
        "physical_height": 8 * atlas_scale,
        "atlas_scale": atlas_scale,
        "lower_ascii": [metrics[ord("a") + index] for index in range(26)],
        "upper_ascii": [metrics[ord("A") + index] for index in range(26)],
        "extended": [metrics[0xDF + index] for index in range(30)],
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def write_original_fonts(
    source: Path,
    output: Path,
    preview: Path | None,
    normalize_lowercase_yu: bool,
) -> None:
    atlas, sheets = load_atlas(source)
    if normalize_lowercase_yu:
        # ViT's 0xEA cell is the dedicated lowercase Cyrillic "ю".  Its
        # original six-pixel outline is nearly indistinguishable from 0xDF
        # (uppercase "Ю") after integer scaling.  Keep the original stroke
        # values and x-height, but compact the bowl to five pixels.
        font_c = next(sheet for sheet in sheets if sheet.path.name == "FONTC.TIM")
        x = 8
        y = 40
        background = font_c.data[font_c.pixel_offset + y * font_c.width + x]
        stroke = font_c.data[font_c.pixel_offset + (y + 3) * font_c.width + x]
        edge = font_c.data[font_c.pixel_offset + (y + 2) * font_c.width + x + 2]
        pattern = (
            (background, background, background, background, background, background),
            (background, background, background, background, background, background),
            (stroke, background, edge, stroke, edge, background),
            (stroke, background, stroke, background, stroke, background),
            (stroke, stroke, stroke, background, stroke, background),
            (stroke, background, stroke, background, stroke, background),
            (stroke, background, edge, stroke, edge, background),
            (background, background, background, background, background, background),
        )
        for row, pixels in enumerate(pattern):
            start = font_c.pixel_offset + (y + row) * font_c.width + x
            font_c.data[start : start + len(pixels)] = bytes(pixels)
    output.mkdir(parents=True, exist_ok=True)
    for sheet in sheets:
        (output / sheet.path.name).write_bytes(sheet.data)
    if preview:
        atlas, _ = load_atlas(output)
        preview.parent.mkdir(parents=True, exist_ok=True)
        atlas.resize((512, 512), Image.Resampling.NEAREST).save(preview)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--contact", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--preview", type=Path)
    parser.add_argument("--font", type=Path)
    parser.add_argument("--font-size", type=int, default=16)
    parser.add_argument("--font-weight", type=int, default=600)
    parser.add_argument("--atlas-scale", type=int, choices=(1, 2), default=2)
    parser.add_argument("--metrics", type=Path)
    parser.add_argument("--normalize-lowercase-yu", action="store_true")
    arguments = parser.parse_args()
    metrics = None
    if arguments.font:
        if not arguments.output:
            parser.error("--font requires --output")
        metrics = write_regenerated_fonts(
            arguments.source,
            arguments.output,
            arguments.font,
            arguments.font_size,
            arguments.font_weight,
            arguments.atlas_scale,
        )
    elif arguments.output:
        write_original_fonts(
            arguments.source,
            arguments.output,
            None,
            arguments.normalize_lowercase_yu,
        )
    atlas_source = arguments.output or arguments.source
    atlas, _ = load_atlas(atlas_source)
    active_scale = arguments.atlas_scale if arguments.font else 1
    if arguments.contact:
        write_contact(atlas, arguments.contact, metrics, active_scale)
    if arguments.preview:
        arguments.preview.parent.mkdir(parents=True, exist_ok=True)
        atlas.resize((512, 512), Image.Resampling.NEAREST).save(arguments.preview)
    if arguments.metrics:
        if metrics is None:
            parser.error("--metrics requires --font")
        write_metrics(
            metrics,
            arguments.font.stem,
            arguments.atlas_scale,
            arguments.metrics,
        )
    if not arguments.contact and not arguments.output:
        parser.error("at least one of --contact or --output is required")


if __name__ == "__main__":
    main()
