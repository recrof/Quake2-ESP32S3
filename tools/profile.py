#!/usr/bin/env python3
"""Per function histogram of the "prof" console command samples.

usage: tools/profile.py <console.log> [build/Quake2ESP32S3.elf] [top N]

The samples are the "PROF <pc>..." lines (the last run in the log); the PCs
are mapped to functions with addr2line (the IDF environment must be set up:
. tools/idf_env.sh).
"""
import collections
import subprocess
import sys


def main():
    log = sys.argv[1]
    elf = sys.argv[2] if len(sys.argv) > 2 else "build/Quake2ESP32S3.elf"
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    runs, pcs = [], []
    for line in open(log, errors="replace"):
        i = line.find("PROF ")
        if i < 0:
            continue
        words = line[i:].split()
        if words[1] == "END":
            runs.append(pcs)
            pcs = []
        else:
            pcs += [int(w, 16) for w in words[1:]
                    if len(w) == 8 and all(c in "0123456789abcdef" for c in w)]
    samples = runs[-1] if runs else pcs
    counts = collections.Counter(samples)
    addrs = sorted(counts)
    out = subprocess.run(["xtensa-esp32s3-elf-addr2line", "-f", "-e", elf] + ["0x%x" % a for a in addrs],
                         capture_output=True, text=True).stdout.splitlines()
    funcs = collections.Counter()
    for k, a in enumerate(addrs):
        funcs[out[2 * k] if 2 * k < len(out) else "?"] += counts[a]
    total = len(samples)
    print("%d samples" % total)
    for name, n in funcs.most_common(top):
        print("%5.1f%%  %s" % (100.0 * n / total, name))


if __name__ == "__main__":
    main()
