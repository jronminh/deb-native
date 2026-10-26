#!/usr/bin/env python3
"""Find glibc's cancellable syscall site in a libc, at build time.

Every cancellable wrapper (open64, read, write, ...) sets the syscall number in
x6 and branches to one function (`__syscall_cancel_arch`) that does
`mov x8, x6` and then a single `svc #0` shared by all of them.  The shim's
runtime scan looks for `mov x8,#nr; svc` and cannot see it (the number comes from
x6, and instructions sit in between), so its offset is found here, from the
known libc, and handed to the shim.

Usage: find-cancellable.py LIBC
Prints one line and exits 0, or a message and exits 1.

The signature (`mov x8, x6` within a short window before a `svc`) is glibc's for
the aarch64 `syscall_cancel.S`; a libc that changes it is reported as not found,
not guessed.  See docs/reference/experiments.md, E9.
"""
import struct
import sys

MOV_X8_X6 = 0xaa0603e8          # mov x8, x6
SVC = 0xd4000001                # svc #0
WINDOW = 30                     # instructions to look ahead for the svc
PT_LOAD = 1
PF_X = 1


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    data = open(sys.argv[1], "rb").read()
    if data[:4] != b"\x7fELF" or data[4] != 2:
        print("find-cancellable: not an ELF64", file=sys.stderr)
        return 1
    e_phoff = struct.unpack_from("<Q", data, 0x20)[0]
    e_phentsize = struct.unpack_from("<H", data, 0x36)[0]
    e_phnum = struct.unpack_from("<H", data, 0x38)[0]
    base = min(struct.unpack_from("<Q", data, e_phoff + i * e_phentsize + 16)[0]
               for i in range(e_phnum))
    hits = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, p_flags = struct.unpack_from("<II", data, off)
        p_offset, p_vaddr, _pad, p_filesz = struct.unpack_from("<QQQQ", data, off + 8)
        if p_type != PT_LOAD or not (p_flags & PF_X):
            continue
        seg = data[p_offset:p_offset + p_filesz]
        w = struct.unpack_from("<%dI" % (len(seg) // 4), seg)
        for k in range(len(w)):
            if w[k] != MOV_X8_X6:
                continue
            for j in range(1, WINDOW):
                if k + j < len(w) and w[k + j] == SVC:
                    hits.append((p_vaddr + (k + j) * 4, j, p_vaddr + k * 4))
                    break
    if len(hits) != 1:
        print("find-cancellable: %d candidates, expected 1" % len(hits), file=sys.stderr)
        return 1
    svc_vaddr, j, mov_vaddr = hits[0]
    print("cancellable svc: offset 0x%x (vaddr 0x%x, %d insns after mov x8,x6 at 0x%x)"
          % (svc_vaddr - base, svc_vaddr, j, mov_vaddr))
    return 0


sys.exit(main())
