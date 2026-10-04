#!/usr/bin/env python3
"""Print SINS journal.sins records (same format as pkg/libsystemd/journal.c)."""
from __future__ import annotations

import os
import struct
import sys

MAGIC = 0x534A4E49  # SINJ LE
VER = 1


def path_default() -> str:
    return os.environ.get("SINS_JOURNAL_FILE", "/var/log/sins-journal/journal.sins")


def dump(p: str) -> None:
    try:
        data = open(p, "rb").read()
    except OSError as e:
        print(e, file=sys.stderr)
        sys.exit(1)
    off = 0
    n = 0
    while off + 24 <= len(data):
        magic, ver, rt, mon, nf = struct.unpack_from("<IIQQI", data, off)
        if magic != MAGIC or ver != VER:
            break
        off += 24
        fields = []
        for _ in range(nf):
            if off + 4 > len(data):
                return
            kl, vl = struct.unpack_from("<HH", data, off)
            off += 4
            if off + kl + vl > len(data):
                return
            key = data[off : off + kl].decode("utf-8", "replace")
            val = data[off + kl : off + kl + vl].decode("utf-8", "replace")
            off += kl + vl
            fields.append(f"{key}={val}")
        n += 1
        print(f"--- entry {n} realtime_usec={rt} monotonic_usec={mon}")
        for f in fields:
            print(f"  {f}")
    if n == 0 and len(data) > 0:
        print("no valid records (truncated or empty file)", file=sys.stderr)


def main() -> None:
    p = sys.argv[1] if len(sys.argv) > 1 else path_default()
    dump(p)


if __name__ == "__main__":
    main()
