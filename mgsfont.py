#!/usr/bin/env python3
"""The dialogue font of MGS2 Master Collection.

The font is a gzip stream named MGS_Font_nht.raw inside the executable's data (not in
the encrypted code). Unpacked: magic "TNOF", cell size, a (codepoint, offset) table, then
per glyph a 5-byte header (width, height, x offset, y offset, advance) and 8-bit coverage.

  python3 mgsfont.py "METAL GEAR SOLID2.exe" font_original.raw
"""
import struct
import sys
import zlib
from pathlib import Path
from typing import NamedTuple

BLOB_NAME = b"MGS_Font_nht.raw"
GZIP_HEADER = 10  # bytes of gzip header before the stored file name


class Glyph(NamedTuple):
    width: int
    height: int
    x_offset: int
    y_offset: int
    advance: int
    coverage: bytes


def extract(executable: bytes) -> bytes:
    """The unpacked font blob from the game's executable."""
    at = executable.find(BLOB_NAME)
    if at < GZIP_HEADER:
        raise ValueError("font not found: is this METAL GEAR SOLID2.exe?")
    return zlib.decompressobj(zlib.MAX_WBITS | 16).decompress(executable[at - GZIP_HEADER:])


def read(raw: bytes) -> dict[int, Glyph]:
    if raw[:4] != b"TNOF":
        raise ValueError("not a font blob")
    table, count = struct.unpack_from("<II", raw, 0x10)
    glyphs = {}
    for index in range(count):
        codepoint, at = struct.unpack_from("<II", raw, table + 8 * index)
        width, height, x_offset, y_offset, advance = struct.unpack_from("<BBbbB", raw, at)
        glyphs[codepoint] = Glyph(width, height, x_offset, y_offset, advance, raw[at + 5:at + 5 + width * height])
    return glyphs


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    raw = extract(Path(sys.argv[1]).read_bytes())
    Path(sys.argv[2]).write_bytes(raw)
    print(f"{len(read(raw))} glyphs -> {sys.argv[2]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
