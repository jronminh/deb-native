# Findings: a real sed-delimiter bug that silently broke everything (2026-09-25)

The single highest-value bug found today, by far: not a design flaw, a
plain shell scripting mistake that made the whole shebang/wrapper
mechanism look broken for hours of testing.

## The bug

```sh
flag=$(printf '%s' "$shebang" | sed -E 's#^#![[:space:]]*/bin/...#\2#')
```

Using `#` as the `sed` delimiter, in a pattern that itself starts with a
**literal `#`** (matching a shebang's own `#!`). `sed` parses the
delimiter positionally, not semantically — the very first `#` after `s`
closes the "pattern" section immediately (as `^`, an empty-ish pattern),
and the real pattern text (`!...`) gets read as something sed can't
parse as a valid trailing section, producing:

```
sed: -e expression #1, char 78: unknown option to `s'
```

Under this script's `set -eu`, this single failing `sed` call **aborted
the entire script**, mid-loop, for whichever maintainer script happened
to be processed at that point (alphabetically before `openssl.postinst`,
in every run — `base-files`, `base-passwd`, `dash`, `debconf`,
`debianutils`, all sort earlier than `openssl`). Every file
alphabetically *after* the crash point silently never got touched, in
*every single test run* — indistinguishable, from the outside, from "the
shebang mechanism just doesn't work for this case," which is exactly what
several rounds of testing today concluded before this was found.

## How it was actually found

Not by reading the code harder. By directly invoking
`patch-maintainer-scripts.sh` by hand against an already-built test
prefix and looking at its own exit code and stderr — `sed: unknown option
to 's'`, `exit: 1` — instead of only ever running it embedded inside the
full multi-minute bootstrap pipeline (where a `|| true` at the call site
swallowed the failure silently, downstream errors in the log looked like
independent, unrelated bugs, and there was no way to tell "this script
crashed" from "this script ran and decided not to do anything here").

## The fix

Use a delimiter that appears in neither the pattern nor the replacement —
`,` here (`|` was tried first and rejected: it's the regex alternation
operator inside `(sh|bash|dash)`, so it would have broken delimiter
parsing the same way, just for a different reason):

```sh
flag=$(printf '%s' "$shebang" | sed -E 's,^#![[:space:]]*/bin/...,\2,')
```

## What this unblocked

With this one fix, `openssl` went from permanently stuck (`iF`, `ln:
failed... /usr/lib/ssl`) to fully configuring correctly — its shebang
finally got rewritten to the `dn-dash` wrapper (with its own `-e` flag
correctly preserved, a separate fix made earlier the same session:
`grep -qE '^#!\s*/bin/(sh|bash|dash)\s*$'` also never matched
`#!/bin/sh -e` at all, since `\s` is PCRE, not POSIX ERE, and the pattern
required nothing after the interpreter name). `dash`, `debianutils`, and
`mawk` also reached fully configured (`ii`) as a direct result, since
their own control scripts were among the earlier casualties of the same
crash.

## Lesson for the rest of this project

**Test the actual failing tool in isolation before concluding a mechanism
doesn't work**, especially for anything wrapped in `set -eu` plus a
swallowing `|| true` one level up — the combination is specifically
dangerous because it converts "this script crashed outright" into
silence, indistinguishable from "this script ran and correctly decided
nothing needed doing." Both `patch-deb.sh` and
`patch-maintainer-scripts.sh` had the identical bug (copy-pasted between
them) and both needed the identical fix.
