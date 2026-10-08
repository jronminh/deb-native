#!/usr/bin/env python3
"""Check the syscall catalog (src/syscalls.tsv).

It is the single source of truth the overlay is built from: the build embeds
it in dn-trace verbatim (src/tracer/GNUmakefile -> syscall/dn-catalog.c), which
builds the seccomp filter's gate-IP exemption from it, and dn-glibc is meant
to wire the rows marked glibc=yes.  This checks the catalog is well-formed and
internally consistent, and that it still carries its kernel/Android
provenance -- the list follows the Android kernel and policy, so a missing or
stale provenance block is a real problem, not a nit.

What it checks:
  1. The provenance block (its '# key:' lines) names arch, android, kernel,
     sources and updated -- the catalog is scoped, not universal.
  2. Every data row has the five columns, and each column's value is one it
     is allowed to be.
  3. Invariants: gate=yes implies glibc=yes (a call may be ALLOWed from the
     gate only if dn-glibc already handles it in-process), and implies the
     row is in the path or identity group (nothing else is treated specially).
  4. No syscall is listed twice.
  5. gate=no for every syscall whose dn-glibc wrapper is generated from
     syscalls.list and so does not translate (UNTRANSLATED_WRAPPERS): libc
     issues it from the gate page, so gate=yes would let the untranslated
     call through to the host.
  6. Every line fits dn-trace's embedded parser (LINE_MAX bytes).

Usage: tools/check-syscalls.py [--root DIR]

Exit status: nonzero if any problem was found.
"""
import argparse
import os
import sys

COLUMNS = ["syscall", "group", "handling", "glibc", "gate"]
GROUPS = {"path", "identity"}
YESNO = {"yes", "no"}
HANDLING = {
    "map", "resolve", "fake-stat", "fake-ids", "fake-owner", "link2symlink",
    "fake-xattr", "reverse", "map-sockaddr",
}
PROVENANCE_KEYS = ["arch", "android", "kernel", "sources", "updated"]
# dn-glibc wrappers generated from syscalls.list: no .c to wire, so the
# function (e.g. unlinkat()) passes its path through untranslated even when a
# sibling (unlink()) is wired.  Drop a name once dn-glibc overrides it.
UNTRANSLATED_WRAPPERS = {
    "mkdirat", "unlinkat", "symlinkat", "linkat", "readlinkat", "fchownat",
    "chdir", "chroot", "inotify_add_watch",
    "setxattr", "lsetxattr", "getxattr", "lgetxattr", "listxattr",
    "llistxattr", "removexattr", "lremovexattr",
}
# dn-trace's embedded-catalog parser reads each line into a char[512] (with
# its NUL).
LINE_MAX = 511


def parse(path):
    """Return (provenance:dict, rows:list[dict])."""
    provenance = {}
    rows = []
    with open(path, encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if line.startswith("#"):
                body = line.lstrip("#").strip()
                # "# key: value" -- only the leading key: lines are keys.
                if ":" in body:
                    key, _, value = body.partition(":")
                    key = key.strip()
                    if key in PROVENANCE_KEYS and key not in provenance:
                        provenance[key] = value.strip()
                continue
            if not line.strip():
                continue
            rows.append((lineno, line.split("\t")))
    return provenance, rows


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    args = ap.parse_args()

    path = os.path.join(args.root, "src", "syscalls.tsv")
    problems = []

    if not os.path.isfile(path):
        print(f"== syscall catalog ==\n  E: {path} not found")
        return 1

    provenance, rows = parse(path)

    print("== Provenance ==")
    for key in PROVENANCE_KEYS:
        if key not in provenance:
            problems.append(f"provenance is missing '{key}:'")
    if not problems:
        print("  arch={arch} android={android}".format(**provenance))
        print("  kernel={kernel}".format(**provenance))
        print("  updated={updated}".format(**provenance))
    print()

    print("== Rows ==")
    seen = {}
    with open(path, encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            if len(line.rstrip("\n").encode()) > LINE_MAX:
                problems.append(f"line {lineno}: longer than {LINE_MAX} bytes "
                                "(dn-trace's embedded parser would reject the catalog)")
    for lineno, fields in rows:
        if len(fields) != len(COLUMNS):
            problems.append(f"line {lineno}: expected {len(COLUMNS)} columns, got {len(fields)}")
            continue
        row = dict(zip(COLUMNS, fields))
        name = row["syscall"]

        if name in seen:
            problems.append(f"line {lineno}: '{name}' already listed on line {seen[name]}")
        seen[name] = lineno

        if row["group"] not in GROUPS:
            problems.append(f"line {lineno}: bad group '{row['group']}'")
        if row["glibc"] not in YESNO:
            problems.append(f"line {lineno}: bad glibc '{row['glibc']}'")
        if row["gate"] not in YESNO:
            problems.append(f"line {lineno}: bad gate '{row['gate']}'")
        for op in row["handling"].split(","):
            if op and op not in HANDLING:
                problems.append(f"line {lineno}: unknown handling op '{op}'")

        if row["gate"] == "yes" and row["glibc"] != "yes":
            problems.append(f"line {lineno}: '{name}' is gate=yes but glibc=no "
                            "(a gate-issued call must already be handled in-process)")
        if row["gate"] == "yes" and name in UNTRANSLATED_WRAPPERS:
            problems.append(f"line {lineno}: '{name}' is gate=yes but its dn-glibc wrapper "
                            "does not translate (syscalls.list-generated)")
        if row["gate"] == "yes" and row["group"] == "other":
            problems.append(f"line {lineno}: '{name}' is gate=yes but group 'other'")

    if not problems:
        n_gate = sum(1 for _, f in rows if len(f) == 5 and f[4] == "yes")
        print(f"  clean -- {len(rows)} rows, {n_gate} gate-exempt")
    print()

    if problems:
        print("== Problems ==")
        for p in problems:
            print(f"  {p}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
