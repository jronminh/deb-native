#!/usr/bin/env python3
"""Pick a random, in-scope survey sample from a Debian Packages index.

Scope is docs/standard.md's (the same split as sudo-less): the package's
Section must be `user`, and none of the signals that make it the admin's:
Priority required/important/standard, Essential, a dependency on adduser
(a system user) or init-system-helpers (a system service). Setuid files
cannot be seen in the index; the survey reports them if they matter.

The sample is spread evenly over the in-scope sections: every section gets
N // sections packages, and the remainder goes to randomly chosen sections.
The same seed gives the same sample.

Usage:  scripts/survey-sample.py Packages.xz [N] [SEED] > list.tsv
Output: section <TAB> package   (header line "section<TAB>package")
Run it where Python is (the sampling needs no device); the survey itself
(scripts/survey-prefix.sh) only reads the TSV.
"""
import lzma, gzip, random, sys

IN_SCOPE = set("""libs libdevel devel debug introspection vcs
python perl ruby rust golang haskell javascript java php ocaml lisp gnu-r interpreters
utils text editors shells doc fonts localization tex
science math graphics sound video games electronics hamradio education embedded
x11 gnome kde xfce web comm""".split())
ADMIN_DEPS = {"adduser", "init-system-helpers"}
ADMIN_PRIORITY = {"required", "important", "standard"}
FIELDS = {"Package", "Section", "Priority", "Essential", "Depends", "Pre-Depends"}


def records(path):
    op = lzma.open if path.endswith(".xz") else gzip.open if path.endswith(".gz") else open
    rec = {}
    with op(path, "rt", errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line:
                if rec:
                    yield rec
                rec = {}
                continue
            if line[0] in " \t":
                continue
            k, _, v = line.partition(": ")
            if k in FIELDS:
                rec[k] = v
    if rec:
        yield rec


def dep_names(field):
    for alt in field.replace("|", ",").split(","):
        name = alt.strip().split(" ")[0].split(":")[0]
        if name:
            yield name


def main():
    path = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 100
    seed = sys.argv[3] if len(sys.argv) > 3 else "deb-native-0.2.0"
    per = {}
    for r in records(path):
        sec = r.get("Section", "").split("/")[-1]
        if sec not in IN_SCOPE:
            continue
        if r.get("Priority") in ADMIN_PRIORITY or r.get("Essential") == "yes":
            continue
        deps = set(dep_names(r.get("Depends", "") + "," + r.get("Pre-Depends", "")))
        if deps & ADMIN_DEPS:
            continue
        per.setdefault(sec, set()).add(r["Package"])

    rng = random.Random(seed)
    secs = sorted(per)
    quota = {s: n // len(secs) for s in secs}
    for s in rng.sample(secs, n % len(secs)):
        quota[s] += 1
    print("section\tpackage")
    for s in secs:
        for p in sorted(rng.sample(sorted(per[s]), min(quota[s], len(per[s])))):
            print(f"{s}\t{p}")
    print(f"# {len(secs)} sections, {sum(len(v) for v in per.values())} candidates, "
          f"seed {seed!r}", file=sys.stderr)


if __name__ == "__main__":
    main()
