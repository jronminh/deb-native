#!/usr/bin/env python3
"""Pick an in-scope package sample from a Debian binary-arm64 Packages index.

Implements the section-based scope in docs/standard.md: every package whose
Debian `Section` is in scope, minus the tuned size window, is a candidate.
Used to build the corpus the libc-shim coverage (docs/shim-coverage.md) is
measured against.

Usage:  python3 scripts/scope-sample.py Packages.gz > sel.tsv 2> scope.txt
Env:    PER_SECTION (default 6), MIN_SIZE, MAX_SIZE, MAX_TOTAL
Output: TSV  package <TAB> url <TAB> size ; the stats go to stderr.
"""
import gzip, sys, os

IN_SCOPE = set("""libs libdevel devel debug introspection vcs
python perl ruby rust golang haskell javascript java php ocaml lisp gnu-r interpreters
utils text editors shells doc fonts localization tex
science math graphics sound video games electronics hamradio education embedded
x11 gnome kde xfce web comm""".split())

OUT_OF_SCOPE = set("admin kernel net mail database httpd tasks metapackages".split())
UNDECIDED = set("cli-mono gnustep misc news oldlibs otherosfs zope".split())

PER_SECTION = int(os.environ.get("PER_SECTION", "6"))
MIN_SIZE = int(os.environ.get("MIN_SIZE", "5000"))
MAX_SIZE = int(os.environ.get("MAX_SIZE", "1500000"))
MAX_TOTAL = int(os.environ.get("MAX_TOTAL", "300"))
BASE = "https://deb.debian.org/debian/"

FIELDS = {"Package", "Section", "Filename", "Size", "Priority", "Essential",
          "Depends", "Architecture"}


def parse(path):
    out, rec = [], {}
    with gzip.open(path, "rt", errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line:
                if rec:
                    out.append(rec)
                rec = {}
                continue
            if line[0] in " \t":          # continuation (Description)
                continue
            k, _, v = line.partition(": ")
            if k in FIELDS:
                rec[k] = v
    if rec:
        out.append(rec)
    return out


def main():
    index = sys.argv[1] if len(sys.argv) > 1 else "Packages.gz"
    per, counts = {}, {}
    for r in parse(index):
        sec = r.get("Section", "unknown").split("/")[-1]
        counts[sec] = counts.get(sec, 0) + 1
        if sec not in IN_SCOPE:
            continue
        try:
            size = int(r.get("Size", "0"))
        except ValueError:
            continue
        if MIN_SIZE <= size <= MAX_SIZE:
            per.setdefault(sec, []).append(r)

    sel = []
    for sec in sorted(per):
        for r in per[sec][:PER_SECTION]:
            sel.append(r)
            if len(sel) >= MAX_TOTAL:
                break
        if len(sel) >= MAX_TOTAL:
            break

    total = sum(int(r.get("Size", "0")) for r in sel)
    print(f"# in-scope sections: {len([s for s in counts if s in IN_SCOPE])}, "
          f"selected: {len(sel)} packages, {total / 1e6:.1f} MB", file=sys.stderr)
    print(f"# out-of-scope sections present: "
          f"{', '.join(sorted(s for s in OUT_OF_SCOPE if s in counts))}",
          file=sys.stderr)
    for sec in sorted(per):
        print(f"#   {sec}: {len(per[sec])} candidates of {counts[sec]}", file=sys.stderr)

    for r in sel:
        fn = r.get("Filename")
        if fn:
            sec = r.get("Section", "unknown").split("/")[-1]
            print(f"{sec}\t{r['Package']}\t{BASE}{fn}\t{r.get('Size', '?')}")


if __name__ == "__main__":
    main()
