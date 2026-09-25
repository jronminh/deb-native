#!/usr/bin/env python3
"""Draw a random, reproducible sample of Debian packages for scripts/survey.sh,
mirroring sudo-less's own survey methodology (docs/survey-2026-09.md):
leave out required/important/standard-priority packages (base install,
not what a user installs by hand), metapackages, transitional/dummy
packages, and anything too large to download quickly for a survey.

Usage:
    curl -fsSL https://deb.debian.org/debian/dists/stable/main/binary-arm64/Packages.gz \
      | gunzip > Packages
    scripts/sample-packages.py Packages > sample.tsv

Output: "section<TAB>package<TAB>pool/path/to/file.deb" lines, ready for
scripts/survey.sh. At most 2 packages per section, capped at N total
(default 30 — small enough to actually run in one sitting; sudo-less's
own real-installation round used 129).
"""
import re
import random
import sys

def field(paragraph, name):
    m = re.search(rf"^{name}: (.+)$", paragraph, re.M)
    return m.group(1) if m else None

def sample(packages_file, seed=20260925, per_section=2, total=30, max_size=5_000_000):
    data = open(packages_file, errors="ignore").read()
    pool = []
    for p in data.split("\n\n"):
        if not p.strip():
            continue
        name = field(p, "Package")
        if not name:
            continue
        if (field(p, "Priority") or "").lower() in ("required", "important", "standard"):
            continue
        section = field(p, "Section") or "?"
        if section == "metapackages":
            continue
        desc = (field(p, "Description") or "").lower()
        if "transitional" in desc or "dummy" in desc:
            continue
        size = field(p, "Size")
        if size and int(size) > max_size:
            continue
        fn = field(p, "Filename")
        if not fn:
            continue
        pool.append((section, name, fn))

    random.seed(seed)
    random.shuffle(pool)
    seen = {}
    out = []
    for s, n, fn in pool:
        if seen.get(s, 0) >= per_section:
            continue
        seen[s] = seen.get(s, 0) + 1
        out.append((s, n, fn))
        if len(out) >= total:
            break
    return out

if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    for s, n, fn in sample(sys.argv[1]):
        print(f"{s}\t{n}\t{fn}")
