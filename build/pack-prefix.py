#!/usr/bin/env python3
"""Apply the build invariants to a staged prefix tree and write its .dn/.

docs/spec/prefix-contract.md, "Build invariants". Run on a copy of a built
prefix (package-prefix.sh stages one); the tree is changed in place:

  - every glibc ELF whose PT_INTERP is ROOT/<loader> gets a CAPACITY-byte
    interpreter (a placeholder reserved with dn-elf, then the real path and a
    NUL written at its start), and its offset is recorded;
  - ELFs with any other interpreter (the system's Bionic linker) are left
    alone;
  - text files that name ROOT are listed; a binary that names ROOT anywhere
    but in PT_INTERP fails the build;
  - absolute symlinks into ROOT become relative; ld.so.cache is removed;
  - home/, root -> home, mnt -> ../mnt and
    etc/resolv.conf -> ../../app/etc/resolv.conf are made;
  - .dn/contract, .dn/baked-paths, .dn/packages and .dn/relocate.sh are
    written.

Usage: pack-prefix.py TREE --root ROOT --name NAME [--desc TEXT]
                      [--version V] [--relocate SCRIPT] [--dn-elf PATH]
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys

LOADER = "usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"
CAPACITY = 256
PT_INTERP = 3


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
    ap.add_argument("--root", required=True, help="the absolute path the tree's files name")
    ap.add_argument("--name", required=True)
    ap.add_argument("--desc", default="")
    ap.add_argument("--version", default="")
    ap.add_argument("--relocate", help="relocate.sh to copy in (default: core/runtime/relocate.sh)")
    ap.add_argument("--dn-elf", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "core/native/.build-glibc/dn-elf"),
        help="dn-elf binary that reserves the PT_INTERP capacity")
    ap.add_argument("--install", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "core/runtime/install-prefix.sh"),
        help="host-side activation script, copied to .dn/install.sh")
    ap.add_argument("--bootstrap", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "core/runtime/bootstrap-prefix.sh"),
        help="prefix-side completion script, copied to .dn/bootstrap.sh")
    a = ap.parse_args()

    T = os.path.abspath(a.tree)
    ROOT = a.root.rstrip("/")
    rootb = ROOT.encode()
    build_ld = ROOT + "/" + LOADER
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

    elf, text, bad, relinked, foreign_links = [], [], [], 0, []
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
                if cur != build_ld:
                    if rootb in open(p, "rb").read():
                        bad.append((rel, f"interpreter {cur}, and names the build path"))
                    continue            # Bionic or another loader: not ours
                if size != CAPACITY:
                    placeholder = "/" + "x" * (CAPACITY - 2)   # CAPACITY-1 chars + NUL
                    subprocess.run([a.dn_elf, "set-interp", p, placeholder], check=True)
                    off, size, _ = elf_interp(p)
                    if size != CAPACITY:
                        sys.exit(f"pack-prefix: {rel}: PT_INTERP is {size} bytes after dn-elf, wanted {CAPACITY}")
                with open(p, "r+b") as f:
                    f.seek(off)
                    f.write(build_ld.encode() + b"\0" * (CAPACITY - len(build_ld)))
                if open(p, "rb").read().count(rootb) != 1:
                    bad.append((rel, "names the build path outside PT_INTERP"))
                elf.append((rel, off, CAPACITY))
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
    with open(os.path.join(dn, "baked-paths"), "w") as f:
        for rel, off, cap in sorted(elf):
            f.write(f"elf\t{rel}\t{off}\t{cap}\n")
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

    reloc = a.relocate or os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "core", "runtime", "relocate.sh")
    shutil.copyfile(reloc, os.path.join(dn, "relocate.sh"))
    os.chmod(os.path.join(dn, "relocate.sh"), 0o755)

    # The two activation scripts the contract names: install (host side) and
    # bootstrap (prefix side). Copied in, so the host needs no per-target code.
    for src, dst in ((a.install, "install.sh"), (a.bootstrap, "bootstrap.sh")):
        shutil.copyfile(src, os.path.join(dn, dst))
        os.chmod(os.path.join(dn, dst), 0o755)

    size_mib = 0
    for dp, dns, fns in os.walk(T):
        for n in fns:
            p = os.path.join(dp, n)
            if not os.path.islink(p):
                size_mib += os.path.getsize(p)
    size_mib = size_mib // (1024 * 1024) + 1
    lines = ["# dn prefix contract (docs/spec/prefix-contract.md)",
             "contract=1", f"name={a.name}"]
    if a.desc:
        lines.append(f"desc={a.desc}")
    if a.version:
        lines.append(f"version={a.version}")
    lines += ["arch=aarch64", f"root={ROOT}", f"loader={LOADER}",
              "relocate=.dn/relocate.sh", "install=.dn/install.sh",
              "bootstrap=.dn/bootstrap.sh",
              "entry=usr/bin/dn-shell -i", f"size={size_mib}"]
    with open(os.path.join(dn, "contract"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"pack-prefix: {a.name}: {len(elf)} elf, {len(text)} text, "
          f"{relinked} links made relative, {size_mib} MiB")


if __name__ == "__main__":
    main()
