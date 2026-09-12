#!/usr/bin/env python3
"""One-shot generator for the icon sprite sheet, run once and not part of the build.

The glyphs are authored as bitmaps in `views/qt/icons.cpp` (see that file for why they are data
rather than image files). This script rasterises them into a single PNG that ships as a Qt
resource, so the package contains an actual image while the **source of truth stays the
bitmap table**: the run-time path reads the resource, and `theme.icons_match_the_shipped_sheet`
fails if the two ever disagree.

Why a throwaway script rather than a build step:

  - no image library exists in this build, and adding one to rasterise eight-by-eight grids
    would be a far larger dependency than the feature;
  - the output is 8x8 of palette *indices* per glyph, which is exactly what a pixel-art sheet is
    -- there is nothing to antialias and nothing to compress;
  - an artefact regenerated on every build cannot be reviewed in a diff, and this one is small
    enough to read.

The PNG is 8 bits per pixel, non-interlaced, greyscale, with each pixel being a palette index
(`0xFF` = transparent). Greyscale rather than indexed: the palette lives in `theme.hpp`, so a
PLTE chunk here would be a second copy of it that nothing could keep in step.

Usage: python scripts/make_icon_sheet.py
"""
from __future__ import annotations

import pathlib
import re
import struct
import sys
import zlib

REPO = pathlib.Path(__file__).resolve().parent.parent
SOURCE = REPO / "views" / "qt" / "icons.cpp"
OUTPUT = REPO / "views" / "qt" / "resources" / "icons.png"

TRANSPARENT = 0xFF


def glyphs() -> list[tuple[str, list[str]]]:
    """Every glyph in table order, as (C++ name, rows).

    Table order is the enum's order, which is what `icons::glyph_at` indexes -- so the sheet's
    column n must be glyph n, and that is asserted rather than assumed by the run-time test.
    """
    text = SOURCE.read_text(encoding="utf-8")
    found = re.findall(r"const char\* const (k\w+)\[\] = \{(.*?)\};", text, re.S)
    if not found:
        raise SystemExit("no glyph tables found in icons.cpp")
    out: list[tuple[str, list[str]]] = []
    for name, body in found:
        rows = re.findall(r'"([^"]*)"', body)
        if len(rows) != 8 or any(len(r) != 8 for r in rows):
            raise SystemExit(f"{name}: expected 8 rows of 8 characters")
        out.append((name, rows))
    return out


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    return (struct.pack(">I", len(payload)) + kind + payload
            + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF))


def write_png(path: pathlib.Path, width: int, height: int, pixels: bytes) -> None:
    """An 8-bit greyscale PNG. `pixels` is height rows of width bytes, each a palette index."""
    raw = b"".join(b"\x00" + pixels[y * width:(y + 1) * width] for y in range(height))
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)  # 0 = greyscale
    path.write_bytes(b"\x89PNG\r\n\x1a\n"
                     + png_chunk(b"IHDR", ihdr)
                     + png_chunk(b"IDAT", zlib.compress(raw, 9))
                     + png_chunk(b"IEND", b""))


def main() -> int:
    art = glyphs()
    width = 8 * len(art) + (len(art) - 1)  # one transparent column between glyphs
    height = 8
    pixels = bytearray([TRANSPARENT]) * (width * height)

    for column, (name, rows) in enumerate(art):
        for y, row in enumerate(rows):
            for x, c in enumerate(row):
                if c == ".":
                    continue
                index = int(c)
                if not 0 <= index <= 3:
                    raise SystemExit(f"{name}: ink index {index} is outside the palette")
                pixels[y * width + column * 9 + x] = index

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    write_png(OUTPUT, width, height, bytes(pixels))
    print(f"wrote {OUTPUT.relative_to(REPO)} -- {width}x{height}, {len(art)} glyphs: "
          + ", ".join(n for n, _ in art))
    return 0


if __name__ == "__main__":
    sys.exit(main())
