#!/usr/bin/env python3
import argparse
import struct
import sys
from pathlib import Path

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

import mgsfont

SCALE = 3
RENDER_SIZE = 400
INK = 24


def written(font: ImageFont.FreeTypeFont, char: str, size: tuple[int, int]) -> Image.Image:
    left, top, right, bottom = font.getbbox(char)
    canvas = Image.new("L", (right - left + 8, bottom - top + 8), 0)
    ImageDraw.Draw(canvas).text((4 - left, 4 - top), char, fill=255, font=font)
    box = canvas.point(lambda v: 255 if v > INK else 0).getbbox()
    return canvas.crop(box).resize(size, Image.LANCZOS)


def enlarged(glyph: mgsfont.Glyph, size: tuple[int, int]) -> Image.Image:
    return Image.frombytes("L", (glyph.width, glyph.height), glyph.coverage).resize(size, Image.BICUBIC)


def main() -> int:
    parser = argparse.ArgumentParser(description="Build MGS2HiResFont.bin, the high-resolution glyphs read by MGS2HiResFont.asi.")
    parser.add_argument("game_font", type=Path, help="font blob written by mgsfont.py")
    parser.add_argument("typeface", type=Path, help=".ttf/.otf the Latin glyphs are written in")
    parser.add_argument("output", type=Path)
    parser.add_argument("--game-glyphs", action="store_true", help="also store the game's other glyphs, enlarged")
    args = parser.parse_args()

    glyphs = mgsfont.read(args.game_font.read_bytes())
    font = ImageFont.truetype(str(args.typeface), RENDER_SIZE)
    covered = TTFont(str(args.typeface)).getBestCmap()
    entries, bitmaps, offset, redrawn = [], [], 0, 0
    for codepoint, glyph in sorted(glyphs.items()):
        if not glyph.width * glyph.height:
            continue
        size = (glyph.width * SCALE, glyph.height * SCALE)
        if codepoint in covered:
            image, redrawn = written(font, chr(codepoint), size), redrawn + 1
        elif args.game_glyphs:
            image = enlarged(glyph, size)
        else:
            continue
        entries.append(struct.pack("<IHHI", codepoint, glyph.width, glyph.height, offset))
        bitmaps.append(image.tobytes())
        offset += len(bitmaps[-1])
    args.output.write_bytes(b"MG2F" + struct.pack("<II", SCALE, len(entries)) + b"".join(entries) + b"".join(bitmaps))
    print(f"{len(entries)} glyphs ({redrawn} written in {args.typeface.name}), "
          f"{args.output.stat().st_size >> 10} KB -> {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
