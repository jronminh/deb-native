#!/usr/bin/env python3
"""Check the repo for exactly the kind of drift a reorg leaves behind:
broken markdown links, broken table-of-contents anchors, and scripts
nothing calls any more.

Why this exists: every one of these checks was written ad hoc, by hand,
several times over one long docs/scripts reorganization session -- this
is that work turned into something that doesn't need re-deriving next
time. Run it after moving/renaming/deleting a doc or a script, or just
periodically.

What it checks:
  1. Markdown links -- every bracketed-link target in every .md/.sh/.py
     file, resolved relative to the file it's in. Flags any that don't
     resolve to a real file. (External http(s) links are skipped.)
  2. Table-of-contents anchors -- every bracketed in-page link, checked
     against the headings (## and deeper) in that same file, slugified
     the same way GitHub does it. A doc that follows
     templates/docs.template.md has one of these sections; this catches
     a heading renamed without its Contents entry following along.
  3. Script reachability -- every scripts/**/*.sh and *.py, traced from
     install.sh's own calls, transitively, through $HERE/-style
     invocations. Reports what's NOT reachable this way, for a human to
     judge: some of that is legitimate (bench/, survey/, a packaging
     step meant to be run by hand), and some of it is exactly the "only
     a README mentions it" dead code this project has found and removed
     several times. This check never fails the run by itself -- it's a
     prompt to look, not a verdict.

Usage: scripts/tools/check-repo.py [--root DIR]

Exit status: nonzero if any broken link or broken anchor was found
(check 1/2); the reachability report (check 3) never affects it.
"""
import argparse
import glob
import os
import re
import sys


def slugify(heading: str) -> str:
    """Approximate GitHub's heading-to-anchor algorithm."""
    text = re.sub(r"[`*_]", "", heading)
    text = text.lower()
    text = re.sub(r"[^\w\s-]", "", text)
    text = re.sub(r"\s+", "-", text.strip())
    return text


LINK_RE = re.compile(r"\]\(([^)]+)\)")


def check_links(root: str) -> list[str]:
    problems = []
    files = [
        f
        for f in glob.glob(os.path.join(root, "**/*"), recursive=True)
        if os.path.isfile(f) and os.path.splitext(f)[1] in (".md", ".sh", ".py")
        and "/.git/" not in f
    ]
    for f in files:
        try:
            text = open(f, encoding="utf-8", errors="ignore").read()
        except OSError:
            continue
        headings = [
            l.lstrip("#").strip()
            for l in text.split("\n")
            if re.match(r"^#{2,6} ", l)
        ]
        slugs = set()
        seen: dict[str, int] = {}
        for h in headings:
            s = slugify(h)
            if s in seen:
                seen[s] += 1
                s = f"{s}-{seen[s]}"
            else:
                seen[s] = 0
            slugs.add(s)

        base_dir = os.path.dirname(f)
        for m in LINK_RE.finditer(text):
            target = m.group(1)
            if target.startswith("#"):
                anchor = target[1:]
                if anchor and anchor not in slugs:
                    problems.append(f"{f}: anchor #{anchor} has no matching heading")
                continue
            path_part, _, frag = target.partition("#")
            if not path_part or path_part.startswith(("http://", "https://")):
                continue
            resolved = os.path.normpath(os.path.join(base_dir, path_part))
            if not os.path.exists(resolved):
                problems.append(f"{f}: link to {target!r} -> {resolved} does not exist")
    return problems


def build_script_index(root: str) -> dict[str, str]:
    scripts = {}
    for f in glob.glob(os.path.join(root, "scripts/**/*.sh"), recursive=True) + glob.glob(
        os.path.join(root, "scripts/**/*.py"), recursive=True
    ):
        scripts[os.path.basename(f)] = f
    return scripts


def check_reachability(root: str) -> list[str]:
    basenames = build_script_index(root)
    texts = {}
    for base, path in basenames.items():
        try:
            texts[path] = open(path, encoding="utf-8", errors="ignore").read()
        except OSError:
            texts[path] = ""

    calls: dict[str, set[str]] = {p: set() for p in basenames.values()}
    for path, text in texts.items():
        for base, target in basenames.items():
            if target == path:
                continue
            if re.search(r'[/"]' + re.escape(base) + r'["\s]', text) or re.search(
                r"\$HERE[^\n]*/" + re.escape(base), text
            ):
                calls[path].add(target)

    install_sh = os.path.join(root, "install.sh")
    entrypoints = set()
    if os.path.exists(install_sh):
        install_text = open(install_sh, encoding="utf-8", errors="ignore").read()
        for base, target in basenames.items():
            if re.search(r"/" + re.escape(base) + r'["\s]', install_text):
                entrypoints.add(target)

    reachable = set(entrypoints)
    frontier = list(entrypoints)
    while frontier:
        cur = frontier.pop()
        for nxt in calls.get(cur, ()):
            if nxt not in reachable:
                reachable.add(nxt)
                frontier.append(nxt)

    unreachable = sorted(set(basenames.values()) - reachable)
    report = []
    for path in unreachable:
        callers = [c for c, targets in calls.items() if path in targets]
        tag = "called by " + ", ".join(sorted(callers)) if callers else "NOBODY calls this"
        report.append(f"  {path}  ({tag})")
    return report


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", default=os.getcwd())
    args = ap.parse_args()
    root = os.path.abspath(args.root)

    link_problems = check_links(root)
    print(f"== Links and anchors ({len(link_problems)} problem(s)) ==")
    for p in link_problems:
        print(" ", p)
    if not link_problems:
        print("  clean")

    print()
    print("== Script reachability from install.sh (informational) ==")
    print("  Not reachable doesn't mean dead -- bench/, survey/ and similar")
    print("  are meant to be run by hand. 'NOBODY calls this' and not an")
    print("  intentional standalone tool is the thing worth checking by hand.")
    for line in check_reachability(root):
        print(line)

    return 1 if link_problems else 0


if __name__ == "__main__":
    sys.exit(main())
