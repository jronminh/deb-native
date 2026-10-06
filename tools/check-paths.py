#!/usr/bin/env python3
"""check-paths.py — catch broken `$HERE/…` references in shell scripts.

Scripts find each other through `$HERE/..`, and moving a file (docs/notes/modularize.md
P1) can silently break those; nothing else checks them. This reads each file's
own `HERE=` definition to learn what `$HERE` points at (the script's directory,
or an ancestor when the definition walks up with `..`), then verifies every
literal `$HERE/…` reference resolves on disk.

Scope and limits:
  - `.sh` files only, outside `tests/` (each defines its own fake root).
  - only literal suffixes; a reference with another shell variable or a glob is
    skipped (a glob's parent directory is checked).
  - `$ROOT`/`$REPO` are not checked: several scripts redefine them to a target
    prefix, so their meaning is ambiguous statically.

Usage: check-paths.py
Exit:  0 clean, 1 broken references.
"""

import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
REF = re.compile(r"\$HERE/([^\s\"'`;)}]+)")


def tracked_shells():
    try:
        out = subprocess.check_output(["git", "-C", ROOT, "ls-files"], text=True)
        files = out.splitlines()
    except (OSError, subprocess.CalledProcessError):
        files = []
        for base, dirs, names in os.walk(ROOT):
            dirs[:] = [d for d in dirs if d != ".git"]
            for n in names:
                files.append(os.path.relpath(os.path.join(base, n), ROOT))
    return [p for p in files if p.endswith(".sh") and not p.startswith("tests/")]


def here_base(path, lines):
    """Return the directory $HERE resolves to, from the file's own definition."""
    for line in lines:
        s = line.strip()
        if not s.startswith("HERE="):
            continue
        if "dirname" not in s:
            return None
        ups = s.count("..")
        base = os.path.dirname(os.path.join(ROOT, path))
        for _ in range(ups):
            base = os.path.dirname(base)
        return base
    return None


def main():
    missing = []
    checked = 0
    for path in tracked_shells():
        full = os.path.join(ROOT, path)
        try:
            with open(full, encoding="utf-8") as fh:
                lines = fh.readlines()
        except (UnicodeDecodeError, OSError):
            continue
        base = here_base(path, lines)
        if base is None:
            continue
        checked += 1
        for i, line in enumerate(lines, 1):
            if line.lstrip().startswith("#"):
                continue
            for m in REF.finditer(line):
                suffix = m.group(1)
                if "$" in suffix:
                    continue
                if suffix.endswith(".so"):
                    continue      # a build output, not a committed input
                target = os.path.normpath(os.path.join(base, suffix))
                if "*" in target or "?" in target:
                    target = os.path.dirname(target)
                if not os.path.exists(target):
                    missing.append((path, i, f"$HERE/{suffix}"))

    if not missing:
        print(f"check-paths: OK — $HERE references resolve in {checked} scripts.")
        return 0
    print(f"check-paths: {len(missing)} broken reference(s):")
    for path, line, ref in missing:
        print(f"   {path}:{line}: {ref}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
