#!/usr/bin/env python3
"""Apply the build invariants to a staged prefix tree and write its .dn/.

docs/spec/prefix.md, "Build invariants". Run on a copy of a built
prefix (package-prefix.sh stages one); the tree is changed in place:

  - PT_INTERP is left as the build wrote it: the exec gate runs a program
    through the runtime loader, and dn-trace derives ROOT from its own
    location, so the entry names no root;
  - an ELF that names ROOT anywhere but in its interpreter fails the build;
  - text files that name ROOT are listed (the artifact is relocatable, so
    this list should be empty);
  - absolute symlinks into ROOT become relative; ld.so.cache is removed;
  - home/, root -> home, mnt -> ../mnt and
    etc/resolv.conf -> ../../app/etc/resolv.conf are made;
  - .dn/contract, .dn/packages and .dn/baked-paths are written.

Usage: pack-prefix.py TREE --root ROOT --name NAME [--desc TEXT]
                      [--version V]
"""
import argparse
import os
import shutil
import struct
import sys

LOADER = "usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"
PT_INTERP = 3
# The command the host runs to open a session: dn-trace derives the tree from
# its own location, uses its runtime loader and execs the prefix's init, a
# guest path (docs/spec/prefix.md, "Boot").  It is a full command line, run
# with the prefix root as the working directory, and it names no root.
ENTRY = ("usr/lib/deb-native/dn-trace -- "
         "/usr/bin/bash /usr/lib/deb-native/init.sh")


def elf_interp(path):
    """(offset, size, string) of PT_INTERP, None for a non-ELF or no PT_INTERP."""
    try:
        with open(path, "rb") as f:
            h = f.read(64)
            if len(h) < 64 or h[:4] != b"\x7fELF" or h[4] != 2 or h[5] != 1:
                return None
            phoff, = struct.unpack_from("<Q", h, 0x20)
            phent, phnum = struct.unpack_from("<HH", h, 0x36)
            for i in range(phnum):
                f.seek(phoff + i * phent)
                p = f.read(phent)
                if struct.unpack_from("<I", p, 0)[0] == PT_INTERP:
                    off, = struct.unpack_from("<Q", p, 8)
                    size, = struct.unpack_from("<Q", p, 32)
                    f.seek(off)
                    s = f.read(size).split(b"\0")[0].decode("utf-8", "replace")
                    return off, size, s
    except OSError:
        return None
    return None


def is_elf(path):
    try:
        with open(path, "rb") as f:
            return f.read(4) == b"\x7fELF"
    except OSError:
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tree")
    ap.add_argument("--root", required=True,
                    help="the path the build assumed; asserted not to be named anywhere")
    ap.add_argument("--name", required=True)
    ap.add_argument("--desc", default="")
    ap.add_argument("--version", default="")
    ap.add_argument("--install", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..", "scripts/host/install-prefix.sh"),
        help="host-side activation script, copied to .dn/install.sh")
    ap.add_argument("--entry", default=None,
        help="the command that boots / opens a session, written as the contract's entry= "
             "(default: dn-trace -- the init)")
    a = ap.parse_args()

    T = os.path.abspath(a.tree)
    ROOT = a.root.rstrip("/")
    rootb = ROOT.encode()
    build_ld = ROOT + "/" + LOADER
    entry = a.entry if a.entry is not None else ENTRY
    if not os.path.exists(os.path.join(T, LOADER)):
        sys.exit(f"pack-prefix: {T} has no {LOADER}")

    # ld.so.cache: binary, holds absolute paths; the loader derives its dirs.
    for c in ("usr/etc/ld.so.cache", "etc/ld.so.cache"):
        p = os.path.join(T, c)
        if os.path.lexists(p):
            os.remove(p)

    # The host-facing links and the empty home.
    os.makedirs(os.path.join(T, "home"), exist_ok=True)
    for link, target in (("root", "home"), ("mnt", "../mnt"),
                         ("etc/resolv.conf", "../../app/etc/resolv.conf")):
        p = os.path.join(T, link)
        if os.path.lexists(p) or os.path.isdir(p):
            if os.path.isdir(p) and not os.path.islink(p):
                shutil.rmtree(p)
            else:
                os.remove(p)
        os.symlink(target, p)

    text, bad, relinked, foreign_links = [], [], 0, []
    for dp, dns, fns in os.walk(T):
        rel_dp = os.path.relpath(dp, T)
        if rel_dp == ".dn" or rel_dp.startswith(".dn/"):
            continue
        for n in fns + dns:
            p = os.path.join(dp, n)
            rel = os.path.relpath(p, T)
            if os.path.islink(p):
                t = os.readlink(p)
                if t == ROOT or t.startswith(ROOT + "/"):
                    os.remove(p)
                    os.symlink(os.path.relpath(T + t[len(ROOT):], dp), p)
                    relinked += 1
                elif t.startswith("/data/"):
                    foreign_links.append((rel, t))
                continue
            if not os.path.isfile(p):
                continue
            ip = elf_interp(p)
            if ip is not None:
                off, size, cur = ip
                data = open(p, "rb").read()
                # Runtime v1 leaves PT_INTERP alone.  An ELF that names ROOT
                # must name it only in its interpreter string: dn-trace names
                # ROOT/<loader> (so a poor host can start it) and is correct as
                # built.
                if cur == build_ld:
                    if data.count(rootb) != 1:
                        bad.append((rel, "names the build path outside PT_INTERP"))
                elif rootb in data:
                    bad.append((rel, f"interpreter {cur}, and names the build path"))
                continue
            data = open(p, "rb").read()
            if rootb in data:
                if b"\0" in data or is_elf(p):
                    bad.append((rel, "binary naming the build path"))
                else:
                    text.append(rel)

    if bad:
        for rel, why in bad:
            print(f"pack-prefix: {rel}: {why}", file=sys.stderr)
        sys.exit(f"pack-prefix: {len(bad)} file(s) break the build invariants")
    for rel, t in foreign_links:
        print(f"pack-prefix: warning: {rel} -> {t} points outside the prefix", file=sys.stderr)

    dn = os.path.join(T, ".dn")
    os.makedirs(dn, exist_ok=True)
    # The relocation script was retired: there is no relocation.
    stale = os.path.join(dn, "relocate.sh")
    if os.path.exists(stale):
        os.remove(stale)
    with open(os.path.join(dn, "baked-paths"), "w") as f:
        for rel in sorted(text):
            f.write(f"text\t{rel}\n")

    pk = os.path.join(dn, "packages")
    status = os.path.join(T, "var/lib/dpkg/status")
    if not os.path.exists(pk) and os.path.exists(status):
        with open(status) as s, open(pk, "w") as o:
            for blk in s.read().split("\n\n"):
                kv = dict(l.split(": ", 1) for l in blk.splitlines() if ": " in l and not l.startswith(" "))
                if kv.get("Status", "").endswith(" installed") and "Package" in kv:
                    o.write(f"{kv['Package']}\t{kv.get('Version', '')}\t{kv.get('Architecture', '')}\n")

    # The host-side activation script the contract names.  The prefix's
    # completion is not a script here: the prefix's own init does it on its
    # first boot (docs/spec/prefix.md, "Boot").
    shutil.copyfile(a.install, os.path.join(dn, "install.sh"))
    os.chmod(os.path.join(dn, "install.sh"), 0o755)
    size_mib = 0
    for dp, dns, fns in os.walk(T):
        for n in fns:
            p = os.path.join(dp, n)
            if not os.path.islink(p):
                size_mib += os.path.getsize(p)
    size_mib = size_mib // (1024 * 1024) + 1
    lines = ["# dn prefix contract (docs/spec/prefix.md)",
             "contract=1", f"name={a.name}"]
    if a.desc:
        lines.append(f"desc={a.desc}")
    if a.version:
        lines.append(f"version={a.version}")
    lines += ["arch=aarch64", f"loader={LOADER}",
              "install=.dn/install.sh", f"entry={entry}", f"size={size_mib}"]
    with open(os.path.join(dn, "contract"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"pack-prefix: {a.name}: {len(text)} text, "
          f"{relinked} links made relative, {size_mib} MiB")


if __name__ == "__main__":
    main()
