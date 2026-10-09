#!/usr/bin/env python3
"""Copies files to the SD card of the board over the USB serial port (no need
to take the card out). The firmware must be running (it waits on the serial
console even when the game data is missing).

usage: sdput.py [--port PORT] LOCAL[:REMOTE] ...

REMOTE defaults to /sd/baseq2/<name of LOCAL>. Each block is checked with a
CRC32 computed by the board after writing it.

Note: on macOS, opening the port resets the ESP32-S3: the script waits for the
board to boot before sending.
"""
import argparse
import glob
import os
import sys
import time
import zlib

import serial

BLOCK = 16384


def read_line(s, deadline):
    buf = bytearray()
    while time.time() < deadline:
        c = s.read(1)
        if not c:
            continue
        if c == b"\n":
            return buf.decode("utf-8", "replace").rstrip("\r")
        buf += c
    return None


def wait_for(s, prefixes, timeout):
    deadline = time.time() + timeout
    while True:
        line = read_line(s, deadline)
        if line is None:
            return None
        if line.startswith(prefixes):
            return line


def put(s, local, remote):
    size = os.path.getsize(local)
    s.write(("put %s %d\n" % (remote, size)).encode())
    line = wait_for(s, ("READY", "E "), 10)
    if line != "READY":
        sys.exit("%s: %s" % (remote, line or "no answer"))
    start = time.time()
    done = 0
    with open(local, "rb") as f:
        while done < size:
            block = f.read(BLOCK)
            s.write(block)
            line = wait_for(s, ("K ", "E "), 10)
            if not line or not line.startswith("K "):
                sys.exit("%s: %s at %d" % (remote, line or "timeout", done))
            if int(line[2:], 16) != zlib.crc32(block):
                sys.exit("%s: CRC error at %d" % (remote, done))
            done += len(block)
            elapsed = time.time() - start
            print("\r%s: %d/%d kB, %d kB/s   " % (remote, done // 1024, size // 1024, done / 1024 / max(elapsed, 0.001)),
                  end="", flush=True)
    line = wait_for(s, ("DONE", "E "), 10)
    print("\n%s" % line)


def main():
    ap = argparse.ArgumentParser()
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    ap.add_argument("--port", default=ports[0] if ports else None)
    ap.add_argument("--boot-wait", type=float, default=6.0, help="seconds to wait for the board to boot")
    ap.add_argument("files", nargs="+")
    args = ap.parse_args()
    s = serial.Serial()
    s.port = args.port
    s.baudrate = 115200
    s.timeout = 0.1
    s.dtr = False
    s.rts = False
    s.open()
    time.sleep(args.boot_wait)
    s.reset_input_buffer()
    for spec in args.files:
        local, _, remote = spec.partition(":")
        if not remote:
            remote = "/sd/baseq2/" + os.path.basename(local)
        put(s, local, remote)


if __name__ == "__main__":
    main()
