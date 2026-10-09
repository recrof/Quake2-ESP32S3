"""PNG writer for the screenshots of the serial console (8-bit palette)."""
import struct
import zlib


def write_png(path, width, height, palette, pixels, scale=2):
    rows = []
    for y in range(height):
        line = bytearray()
        for x in range(width):
            c = pixels[y * width + x]
            rgb = bytes(palette[3 * c:3 * c + 3])
            line += rgb * scale
        rows.extend([b"\0" + bytes(line)] * scale)
    raw = b"".join(rows)

    def chunk(kind, data):
        c = struct.pack(">I", len(data)) + kind + data
        return c + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width * scale, height * scale, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)
