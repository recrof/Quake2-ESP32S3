#!/usr/bin/env python3
"""Keeps the USB serial port of the board open (on macOS, opening it resets the
ESP32-S3), logs the console and forwards debug console commands.

usage: serial_daemon.py WORKDIR [--port PORT]

- the console output is appended to WORKDIR/console.log
- lines appended to WORKDIR/commands are sent to the board
- screenshots ("shot" command) are saved as WORKDIR/shot-<n>.png
"""
import argparse
import glob
import os
import sys
import time

import serial

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from shotlib import write_png  # noqa: E402


def open_port(port):
    while True:
        try:
            s = serial.Serial()
            s.port = port
            s.baudrate = 115200
            s.timeout = 0.05
            s.dtr = False
            s.rts = False
            s.open()
            return s
        except Exception:
            time.sleep(0.5)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("workdir")
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    ap.add_argument("--port", default=ports[0] if len(ports) == 1 else None)
    args = ap.parse_args()
    if not args.port:
        # several boards connected: never guess which one
        sys.exit("%d USB serial ports, use --port" % len(ports))
    os.makedirs(args.workdir, exist_ok=True)
    log = open(os.path.join(args.workdir, "console.log"), "a", buffering=1)
    cmd_path = os.path.join(args.workdir, "commands")
    open(cmd_path, "a").close()
    cmd_offset = os.path.getsize(cmd_path)
    shot_index = 0
    while os.path.exists(os.path.join(args.workdir, "shot-%d.png" % shot_index)):
        shot_index += 1
    s = open_port(args.port)
    log.write("--- port opened %s\n" % time.strftime("%H:%M:%S"))
    pending = bytearray()
    shot = None  # [w, h, data]
    while True:
        try:
            data = s.read(65536)
        except Exception:
            log.write("--- port lost %s\n" % time.strftime("%H:%M:%S"))
            try:
                s.close()
            except Exception:
                pass
            time.sleep(1)
            s = open_port(args.port)
            log.write("--- port reopened %s\n" % time.strftime("%H:%M:%S"))
            continue
        pending += data
        while b"\n" in pending:
            raw, _, rest = pending.partition(b"\n")
            pending = bytearray(rest)
            line = raw.decode("utf-8", "replace").rstrip("\r")
            if shot is not None:
                if line.startswith("ENDSHOT"):
                    w, h, buf = shot
                    shot = None
                    if len(buf) == 768 + w * h:
                        path = os.path.join(args.workdir, "shot-%d.png" % shot_index)
                        shot_index += 1
                        write_png(path, w, h, buf[:768], buf[768:])
                        log.write("saved %s\n" % path)
                    else:
                        log.write("screenshot: %d bytes, expected %d\n" % (len(buf), 768 + w * h))
                else:
                    try:
                        shot[2] += bytes.fromhex(line.strip())
                    except ValueError:
                        log.write(line + "\n")
                continue
            if line.startswith("SHOT "):
                _, w, h = line.split()
                shot = [int(w), int(h), bytearray()]
                continue
            log.write(time.strftime("%H:%M:%S ") + line + "\n")
        # new commands
        size = os.path.getsize(cmd_path)
        if size > cmd_offset:
            with open(cmd_path, "rb") as f:
                f.seek(cmd_offset)
                new = f.read()
            cmd_offset = size
            for c in new.decode().splitlines():
                if c.strip():
                    log.write(">>> %s\n" % c.strip())
                    try:
                        s.write((c.strip() + "\n").encode())
                    except Exception:
                        pass
        elif size < cmd_offset:
            cmd_offset = size


if __name__ == "__main__":
    main()
