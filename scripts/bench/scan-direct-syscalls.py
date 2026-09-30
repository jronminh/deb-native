#!/usr/bin/env python3
"""Scan ELFs for direct syscall usage the libc shim cannot see.

Why this exists: a symbol scan only finds binaries that *import* `syscall`.
Go, Rust, static-musl and inline-asm code issue syscalls directly with the
arm64 `svc #0` instruction, which leaves no symbol at all.

Method: parse the ELF, scan only executable (SHF_EXECINSTR) sections at
4-byte alignment for `svc #0` as a cheap candidate filter, then (with
--verify, when objdump is present) disassemble each candidate to remove
literal-pool false positives — a whole-file byte grep matches data too.

Also classifies each ELF: ET_EXEC / PIE executable / shared object, and
flags Go and Rust builds.

Usage: scan-direct-syscalls.py DIR [--verify] [--list]
       scan-direct-syscalls.py DIR --trace-list   # paths needing the tracer
"""
import os
import shutil
import struct
import subprocess
import sys

SVC0 = 0xD4000001               # `svc #0`, little-endian
ET_EXEC, ET_DYN = 2, 3
PT_INTERP = 3
SHF_EXECINSTR = 0x4
OBJDUMP = shutil.which("objdump")


def parse(data):
    """Return a dict of ELF facts, or None."""
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        return None
    e_type = struct.unpack_from("<H", data, 0x10)[0]
    phoff = struct.unpack_from("<Q", data, 0x20)[0]
    phentsize = struct.unpack_from("<H", data, 0x36)[0]
    phnum = struct.unpack_from("<H", data, 0x38)[0]
    has_interp = False
    for i in range(phnum):
        off = phoff + i * phentsize
        if struct.unpack_from("<I", data, off)[0] == PT_INTERP:
            has_interp = True
            break

    shoff = struct.unpack_from("<Q", data, 0x28)[0]
    shentsize = struct.unpack_from("<H", data, 0x3A)[0]
    shnum = struct.unpack_from("<H", data, 0x3C)[0]
    execsecs = []
    for i in range(shnum):
        off = shoff + i * shentsize
        sh_flags = struct.unpack_from("<Q", data, off + 0x8)[0]
        sh_offset = struct.unpack_from("<Q", data, off + 0x18)[0]
        sh_size = struct.unpack_from("<Q", data, off + 0x20)[0]
        if sh_flags & SHF_EXECINSTR:
            execsecs.append((sh_offset, sh_size))

    if e_type == ET_EXEC:
        kind = "exec"                    # classic static/non-PIE executable
    elif has_interp:
        kind = "pie"                     # dynamic executable (PIE)
    else:
        kind = "lib"                     # shared object, or static-PIE
    return {
        "kind": kind,
        "execsecs": execsecs,
        "go": b"Go build ID" in data or b".note.go.buildid" in data,
        "rust": b"rust_eh_personality" in data or b".rustc" in data,
    }


def candidate_svc(data, execsecs):
    for off, size in execsecs:
        end = min(off + size, len(data))
        i = off - (off & 3)
        while i + 4 <= end:
            if struct.unpack_from("<I", data, i)[0] == SVC0:
                return True
            i += 4
    return False


def objdump_svc(path):
    try:
        out = subprocess.run([OBJDUMP, "-d", path], capture_output=True,
                             text=True, errors="replace").stdout
    except OSError:
        return -1
    return sum(1 for line in out.splitlines() if "\tsvc" in line or " svc" in line)


def needs_tracer(data, info, path):
    """True if the ELF issues syscalls the libc shim cannot see: it imports
    the public `syscall` symbol (case 3), or its own code emits `svc` (case 4).
    With no objdump, a byte candidate is kept (conservative)."""
    if b"\x00syscall\x00" in data:
        return True
    if candidate_svc(data, info["execsecs"]):
        return (objdump_svc(path) > 0) if OBJDUMP else True
    return False


def trace_list(root):
    """Print the paths of ELFs in DIR that must be launched under the tracer."""
    for dirpath, _, files in os.walk(root):
        for fn in files:
            p = os.path.join(dirpath, fn)
            try:
                with open(p, "rb") as fh:
                    data = fh.read()
            except OSError:
                continue
            info = parse(data)
            if info and needs_tracer(data, info, p):
                print(p)


def main():
    root = sys.argv[1]
    if "--trace-list" in sys.argv:
        trace_list(root)
        return
    verify = "--verify" in sys.argv
    listing = "--list" in sys.argv
    counts = {"exec": 0, "pie": 0, "lib": 0}
    go = rust = 0
    cand = []
    for dirpath, _, files in os.walk(root):
        for fn in files:
            p = os.path.join(dirpath, fn)
            try:
                with open(p, "rb") as fh:
                    data = fh.read()
            except OSError:
                continue
            info = parse(data)
            if not info:
                continue
            counts[info["kind"]] += 1
            go += info["go"]
            rust += info["rust"]
            if candidate_svc(data, info["execsecs"]):
                cand.append((p, info))
    print(f"ELFs scanned:                 {sum(counts.values())}")
    print(f"  ET_EXEC (non-PIE static):   {counts['exec']}")
    print(f"  PIE executables (dynamic):  {counts['pie']}")
    print(f"  shared objects / static-PIE:{counts['lib']}")
    print(f"Go-marked / Rust-marked:      {go} / {rust}")
    print(f"candidates (svc in .text):    {len(cand)}")
    if verify:
        if not OBJDUMP:
            print("  (objdump not found; candidates unverified)")
        else:
            real = []
            for p, info in cand:
                n = objdump_svc(p)
                if n > 0:
                    real.append((n, info, p))
            print(f"  verified by objdump:        {len(real)}")
            if listing:
                for n, info, p in sorted(real, key=lambda t: -t[0]):
                    tag = info["kind"] + ("+go" if info["go"] else "") + ("+rust" if info["rust"] else "")
                    print(f"  {n:6d}  {tag:12s} {p}")
            return
    if listing:
        for p, info in cand:
            tag = info["kind"] + ("+go" if info["go"] else "") + ("+rust" if info["rust"] else "")
            print(f"  {tag:12s} {p}")


if __name__ == "__main__":
    main()
