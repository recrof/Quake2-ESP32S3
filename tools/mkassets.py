#!/usr/bin/env python3
"""Builds the flash asset image of the ESP32-S3 Quake 2 port from the PAK files.

The models (MD2, without the OpenGL command lists) and the model skins
(decoded PCX) are stored in their in-memory format in the "assets" flash
partition: the engine maps the partition and uses them in place, without
copying them to PSRAM. What does not fit is loaded from the PAK files as usual.

usage: mkassets.py [--size BYTES] OUT.bin PAK...   (PAKs in search order: pak2 pak1 pak0)
       then: esptool.py --chip esp32s3 write_flash 0x1d0000 OUT.bin

Image format (little endian):
  header:  "Q2AS", version (1), number of entries, directory offset
  entries: name (56 bytes, lower case, sorted), offset, size
  data:    16-byte aligned. Skins: width, height (uint16), transparent (uint8),
           3 padding bytes, then the pixels.
"""
import argparse
import struct
import sys

PARTITION_SIZE = 0xE30000
ALIGN = 16
IN_PSRAM = {"models/monsters/boss3/jorg/tris.md2"}


def read_paks(paths):
    files = {}
    for p in paths:
        f = open(p, "rb")
        ident, ofs, ln = struct.unpack("<4sii", f.read(12))
        if ident != b"PACK":
            sys.exit("%s is not a PAK file" % p)
        f.seek(ofs)
        d = f.read(ln)
        for i in range(ln // 64):
            name, o, l = struct.unpack("<56sii", d[i * 64:(i + 1) * 64])
            name = name.split(b"\0")[0].decode().lower()
            if name not in files:      # earlier PAKs override later ones
                files[name] = (p, o, l)
    return files


def read_file(entry):
    p, o, l = entry
    with open(p, "rb") as f:
        f.seek(o)
        return f.read(l)


def strip_md2(data):
    v = list(struct.unpack("<17i", data[:68]))
    num_skins, num_st, num_tris, num_frames = v[5], v[7], v[8], v[10]
    framesize = v[4]
    ofs_skins, ofs_st, ofs_tris, ofs_frames, ofs_glcmds, ofs_end = v[11:17]
    if (ofs_glcmds >= ofs_skins + num_skins * 64 and ofs_glcmds >= ofs_st + num_st * 4
            and ofs_glcmds >= ofs_tris + num_tris * 12 and ofs_glcmds >= ofs_frames + num_frames * framesize):
        v[9] = 0            # num_glcmds
        v[16] = ofs_glcmds  # ofs_end
        return struct.pack("<17i", *v) + data[68:ofs_glcmds]
    return data


def decode_pcx(data):
    xmin, ymin, xmax, ymax = struct.unpack_from("<HHHH", data, 4)
    if data[0] != 0x0A or data[1] != 5 or data[2] != 1 or data[3] != 8 or xmax >= 640 or ymax >= 480:
        return None
    w, h = xmax + 1, ymax + 1
    out = bytearray(w * h + 64)
    src = 128
    for y in range(h):
        # as LoadPCX: a run past the end of a line spills into the next one,
        # which then overwrites it
        x = 0
        while x < w:
            b = data[src]
            src += 1
            if (b & 0xC0) == 0xC0:
                run = b & 0x3F
                b = data[src]
                src += 1
            else:
                run = 1
            for _ in range(run):
                if y * w + x < len(out):
                    out[y * w + x] = b
                x += 1
    out = out[:w * h]
    transparent = 1 if 255 in out else 0
    return struct.pack("<HHB3x", w, h, transparent) + bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", type=lambda x: int(x, 0), default=PARTITION_SIZE)
    ap.add_argument("out")
    ap.add_argument("paks", nargs="+")
    args = ap.parse_args()
    files = read_paks(args.paks)
    candidates = []
    for name, entry in files.items():
        if name.endswith(".md2"):
            candidates.append((name, strip_md2(read_file(entry))))
        elif name.startswith("models/") and name.endswith(".pcx"):
            skin = decode_pcx(read_file(entry))
            if skin:
                candidates.append((name, skin))
    # keep the smallest assets when everything does not fit: the few big ones
    # (boss models) are loaded in PSRAM on the maps that use them. The models
    # in IN_PSRAM are left out first: Jorg is only on boss2, which has room
    # for it, the Makron (on boss1 and boss2) then fits in flash.
    candidates.sort(key=lambda c: (c[0] in IN_PSRAM, len(c[1])))
    entries = []
    stored = {}             # identical files are stored once
    used = 16
    for name, data in candidates:
        need = 64
        if data not in stored:
            need += (len(data) + ALIGN - 1) // ALIGN * ALIGN
        if name in IN_PSRAM or used + need > args.size:
            print("not stored: %s (%d kB)" % (name, len(data) // 1024))
            continue
        entries.append((name, data))
        stored[data] = True
        used += need
    entries.sort(key=lambda e: e[0])
    dir_size = 64 * len(entries)
    offset = (16 + dir_size + ALIGN - 1) // ALIGN * ALIGN
    directory = bytearray()
    blobs = bytearray()
    blob_offset = {}
    for name, data in entries:
        if data not in blob_offset:
            blob_offset[data] = offset + len(blobs)
            blobs += data + b"\0" * (-len(data) % ALIGN)
        directory += struct.pack("<56sii", name.encode(), blob_offset[data], len(data))
    image = struct.pack("<4siii", b"Q2AS", 1, len(entries), 16) + directory
    image += b"\0" * (offset - len(image)) + blobs
    with open(args.out, "wb") as f:
        f.write(image)
    print("%d assets, %d kB of %d kB" % (len(entries), len(image) // 1024, args.size // 1024))


if __name__ == "__main__":
    main()
