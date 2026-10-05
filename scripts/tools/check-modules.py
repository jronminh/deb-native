#!/usr/bin/env python3
"""check-modules.py — enforce the deb-native module boundaries (P0).

Two rules, from MODULARIZE.md:
  1. one home per file: every tracked path matches exactly one module in
     scripts/tools/module-map.tsv;
  2. dependency direction: a `core` file must not reference a
     `build`/`bootstrap`/`adapter`/`product` file.

The edge check is a heuristic: it scans non-comment lines of core files for
identifying regexes of the non-core modules. Known, accepted edges live in
scripts/tools/module-edges.allow (path<TAB>target), so they can be burned down
to an empty file during P2.

Usage: check-modules.py [--list]
Exit:  0 clean, 1 violations.
"""

import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
MAP = os.path.join(HERE, "module-map.tsv")
ALLOW = os.path.join(HERE, "module-edges.allow")

# Regexes that identify a reference to a non-core module, per target module.
# Word/boundary care matters: "install.sh" must not match "apt-install.sh".
EDGE_PATTERNS = {
    "build":     [r"build-path-redirect"],
    "bootstrap": [r"scripts/bootstrap/", r"setup-apt-prefix", r"dn-install-glibc",
                  r"dn-apply-glibc-patch", r"dn-package-glibc",
                  r"dn-package-libc-bin", r"dn-standins"],
    "adapter":   [r"make-shell-interface", r"make-apt-wrappers"],
    "product":   [r"(?<![\w-])install\.sh"],
}
COMMENT_PREFIXES = ("#", "//", "*", "/*", "<!--", ";")


def load_rules():
    rules = []
    with open(MAP, encoding="utf-8") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            module, _, pattern = line.partition("\t")
            if not pattern:
                sys.exit(f"check-modules: bad map line: {line!r}")
            rules.append((module, re.compile(pattern)))
    return rules


def load_allow():
    allowed = set()
    if os.path.exists(ALLOW):
        with open(ALLOW, encoding="utf-8") as fh:
            for line in fh:
                line = line.rstrip("\n")
                if line and not line.startswith("#"):
                    path, _, target = line.partition("\t")
                    allowed.add((path, target))
    return allowed


def tracked_files():
    try:
        out = subprocess.check_output(["git", "-C", ROOT, "ls-files"], text=True)
        return [p for p in out.splitlines() if p]
    except (OSError, subprocess.CalledProcessError):
        files = []
        for base, dirs, names in os.walk(ROOT):
            dirs[:] = [d for d in dirs if d != ".git"]
            for n in names:
                files.append(os.path.relpath(os.path.join(base, n), ROOT))
        return sorted(files)


def classify(path, rules):
    return [m for m, rx in rules if rx.search(path)]


def scan_edges(path):
    """Yield (target, pattern) for a core file that references a non-core module."""
    full = os.path.join(ROOT, path)
    try:
        with open(full, encoding="utf-8") as fh:
            lines = fh.readlines()
    except (UnicodeDecodeError, OSError):
        return
    for line in lines:
        s = line.lstrip()
        if not s or s.startswith(COMMENT_PREFIXES):
            continue
        for target, patterns in EDGE_PATTERNS.items():
            for pat in patterns:
                if re.search(pat, line):
                    yield target, pat


def main():
    list_only = "--list" in sys.argv[1:]
    rules = load_rules()
    allowed = load_allow()
    files = tracked_files()

    by_module = {}
    unmatched, conflicts = [], []
    for path in files:
        mods = classify(path, rules)
        if not mods:
            unmatched.append(path)
        elif len(mods) > 1:
            conflicts.append((path, mods))
        else:
            by_module.setdefault(mods[0], []).append(path)

    if list_only:
        for module in sorted(by_module):
            print(f"== {module} ({len(by_module[module])})")
            for p in sorted(by_module[module]):
                print(f"   {p}")
        return 0

    violations = []
    for path in by_module.get("core", []):
        if path.endswith(".md"):
            continue          # directory READMEs are prose, not dependencies
        for target, pat in scan_edges(path):
            if (path, target) not in allowed:
                violations.append((path, target, pat))

    ok = True
    if unmatched:
        ok = False
        print(f"UNMATCHED ({len(unmatched)}): no module rule matches")
        for p in unmatched:
            print(f"   {p}")
    if conflicts:
        ok = False
        print(f"CONFLICT ({len(conflicts)}): more than one module matches")
        for p, mods in conflicts:
            print(f"   {p}: {', '.join(mods)}")
    if violations:
        ok = False
        print(f"EDGE VIOLATION ({len(violations)}): core -> non-core")
        for p, target, pat in violations:
            print(f"   {p}: {target} via /{pat}/")

    if ok:
        n = sum(len(v) for v in by_module.values())
        mods = ", ".join(f"{m}={len(by_module.get(m, []))}" for m in sorted(by_module))
        print(f"check-modules: OK — {n} files, one home each; no core edge out.")
        print(f"   {mods}")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
