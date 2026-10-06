#!/usr/bin/env python3
"""check-interface.py — enforce the core's public surface (the repo layout P3).

Reads `scripts/prefix/interface.tsv` and checks:
  1. every declared `entry` and `source` path exists;
  2. no non-core module (`bootstrap`/`build`/`adapter`/`product`/`tools`/`tests`)
     references a core script that is not declared as an `entry` (sources and
     artifacts may be referenced freely).

Usage: check-interface.py [--list]
Exit:  0 clean, 1 violation.
"""

import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
MAP = os.path.join(HERE, "module-map.tsv")
IFACE = os.path.join(ROOT, "scripts", "prefix", "interface.tsv")
NONCORE = {"tools", "tests", "patches"}


def tracked_files():
    out = subprocess.check_output(["git", "-C", ROOT, "ls-files"], text=True)
    return [p for p in out.splitlines() if p]


def classify(rules):
    by = {}
    for path in tracked_files():
        mods = [m for m, rx in rules if rx.search(path)]
        if len(mods) == 1:
            by.setdefault(mods[0], []).append(path)
    return by


def load_map():
    rules = []
    with open(MAP, encoding="utf-8") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            module, _, pattern = line.partition("\t")
            rules.append((module, re.compile(pattern)))
    return rules


def load_iface():
    entries, sources, artifacts = [], [], []
    with open(IFACE, encoding="utf-8") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if parts[0] == "entry" and len(parts) >= 2:
                entries.append(parts[1])
            elif parts[0] == "source" and len(parts) >= 2:
                sources.append(parts[1])
            elif parts[0] == "artifact" and len(parts) >= 4:
                artifacts.append((parts[1], parts[2], parts[3]))
    return entries, sources, artifacts


def main():
    list_only = "--list" in sys.argv[1:]
    by = classify(load_map())
    entries, sources, artifacts = load_iface()

    allowed = {os.path.basename(p) for p in entries}
    allowed |= {os.path.basename(p) for p in sources}
    allowed |= {name for name, _, _ in artifacts}
    code_ext = (".sh", ".py", ".c", ".h", ".so")
    core_basenames = {os.path.basename(p) for p in by.get("src", []) + by.get("scripts", [])
                      if p.endswith(code_ext)}

    problems = []
    for p in entries + sources:
        if not os.path.exists(os.path.join(ROOT, p)):
            problems.append(f"declared path missing: {p}")

    refs = []
    for module in NONCORE:
        for path in by.get(module, []):
            if not path.endswith((".sh", ".py")):
                continue
            try:
                text = open(os.path.join(ROOT, path), encoding="utf-8").read()
            except OSError:
                continue
            for base in sorted(core_basenames):
                if base in allowed:
                    continue
                if re.search(r'(?<![\w-])' + re.escape(base) + r'(?![\w.-])', text):
                    refs.append((path, base))
                    problems.append(f"{path}: references core '{base}' (not an entry)")

    if list_only:
        for path, base in refs:
            print(f"   {path}: {base}")
        if not refs:
            print("   (no undeclared core references)")
        return 0

    if problems:
        print(f"check-interface: {len(problems)} problem(s):")
        for p in problems:
            print(f"   {p}")
        return 1
    print(f"check-interface: OK — {len(entries)} entries, {len(sources)} sources, "
          f"{len(artifacts)} artifacts.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
