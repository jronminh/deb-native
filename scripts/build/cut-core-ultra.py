#!/usr/bin/env python3
"""Cut core-ultra (the minimal prefix) out of a core-deb tree.

core-ultra v0 (docs/spec/prefix.md): the files of a few seed
packages (from dpkg's own lists in SRC), the overlay, the small etc files a
shell and a resolver read, and every library their ELFs need (DT_NEEDED),
added until the set is closed. Writes DST/.dn/packages (core-ultra has no
dpkg database). Run before pack-prefix.py, on an unpacked tree.

Usage: cut-core-ultra.py SRC DST
"""
import os
import shutil
import struct
import sys

SEED = ("libc6 libc-bin base-files base-passwd bash dash coreutils debianutils "
        "diffutils dpkg findutils grep gzip hostname init-system-helpers "
        "ncurses-base ncurses-bin perl-base sed tar").split()
EXTRA = ["usr/lib/deb-native/dn-trace",
         "usr/etc/ld.so.conf", "usr/etc/ld.so.conf.d",
         "etc/hosts", "etc/nsswitch.conf", "etc/host.conf", "etc/gai.conf",
         "etc/inputrc", "etc/bash.bashrc", "etc/profile", "etc/profile.d",
         "etc/passwd", "etc/group", "etc/shadow", "etc/ssl",
         "etc/ca-certificates.conf", "usr/share/terminfo", "usr/lib/terminfo",
         "etc/terminfo", "tmp", "run", "opt"]
LIBDIRS = ["usr/lib/aarch64-linux-gnu", "usr/lib"]


def pkg_files(info, p):
    for n in (p + ".list", p + ":arm64.list"):
        f = os.path.join(info, n)
        if os.path.exists(f):
            with open(f) as fh:
                return [l.strip().lstrip("/") for l in fh if l.strip() not in ("", "/.")]
    return []


def needed(path):
    """DT_NEEDED sonames of a 64-bit little-endian ELF."""
    try:
        with open(path, "rb") as f:
            d = f.read()
    except OSError:
        return []
    if d[:4] != b"\x7fELF" or d[4] != 2:
        return []
    phoff, = struct.unpack_from("<Q", d, 0x20)
    phent, phnum = struct.unpack_from("<HH", d, 0x36)
    dyn, loads = None, []
    for i in range(phnum):
        o = phoff + i * phent
        t, = struct.unpack_from("<I", d, o)
        off, vaddr = struct.unpack_from("<QQ", d, o + 8)
        fsz, = struct.unpack_from("<Q", d, o + 32)
        if t == 2:
            dyn = (off, fsz)
        elif t == 1:
            loads.append((vaddr, off, fsz))
    if not dyn:
        return []
    names, strtab = [], None
    for j in range(0, dyn[1], 16):
        tag, val = struct.unpack_from("<qQ", d, dyn[0] + j)
        if tag == 0:
            break
        if tag == 1:
            names.append(val)
        elif tag == 5:
            strtab = val
    so = None
    if strtab is not None:
        so = next((strtab - va + of for va, of, sz in loads if va <= strtab < va + sz), None)
    if so is None:
        return []
    return [d[so + n:d.index(b"\0", so + n)].decode() for n in names]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    SRC, DST = (os.path.abspath(x) for x in sys.argv[1:])
    info = os.path.join(SRC, "var/lib/dpkg/info")
    if not os.path.isdir(info):
        sys.exit(f"cut-core-ultra: {SRC} has no dpkg database")
    if os.path.lexists(DST):
        sys.exit(f"cut-core-ultra: {DST} exists")
    seen = set()

    def add(rel):
        src = os.path.join(SRC, rel)
        if rel in seen or not os.path.lexists(src):
            return
        seen.add(rel)
        dst = os.path.join(DST, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if os.path.islink(src):
            if not os.path.lexists(dst):
                os.symlink(os.readlink(src), dst)
            t = os.readlink(src)
            if not t.startswith("/"):
                add(os.path.normpath(os.path.join(os.path.dirname(rel), t)))
        elif os.path.isdir(src):
            os.makedirs(dst, exist_ok=True)
            if rel in EXTRA:
                for n in os.listdir(src):
                    add(os.path.join(rel, n))
        else:
            shutil.copy2(src, dst)

    os.makedirs(DST)
    for top in ("bin", "lib", "sbin"):
        add(top)
    want = set(EXTRA)
    for p in SEED:
        want.update(pkg_files(info, p))
    for rel in sorted(want):
        add(rel)

    changed = True
    while changed:
        changed = False
        for dp, dns, fns in os.walk(DST):
            for n in fns:
                p = os.path.join(dp, n)
                if os.path.islink(p):
                    continue
                for lib in needed(p):
                    hit = next((os.path.join(L, lib) for L in LIBDIRS
                                if os.path.lexists(os.path.join(SRC, L, lib))), None)
                    if hit and not os.path.lexists(os.path.join(DST, hit)):
                        add(hit)
                        changed = True

    os.makedirs(os.path.join(DST, ".dn"), exist_ok=True)
    with open(os.path.join(SRC, "var/lib/dpkg/status")) as s, \
         open(os.path.join(DST, ".dn/packages"), "w") as o:
        for blk in s.read().split("\n\n"):
            kv = dict(l.split(": ", 1) for l in blk.splitlines() if ": " in l and not l.startswith(" "))
            if kv.get("Package") in SEED:
                o.write(f"{kv['Package']}\t{kv.get('Version', '')}\t{kv.get('Architecture', '')}\n")
    print(f"cut-core-ultra: {len(seen)} paths into {DST}")


if __name__ == "__main__":
    main()
