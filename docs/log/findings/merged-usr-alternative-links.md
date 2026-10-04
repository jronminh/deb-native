# Findings: merged-usr broke some update-alternatives links (2026-10-04)

<!-- template: templates/docs.template.md -->

**Impact: Repo change.** Found by an extended capability sweep (install and run
a broad set of Debian packages): `nc`/`netcat` were not found by name, though
the binary was there.

## Contents

- [Symptom](#symptom)
- [Cause](#cause)
- [Fix](#fix)

## Symptom

`command -v nc` found nothing in the prefix; `nc.openbsd` worked. Its link
was dangling:

```
usr/bin/nc -> ../etc/alternatives/nc      # resolves to usr/etc/alternatives/nc: missing
usr/bin/awk -> ../../etc/alternatives/awk  # correct
```

Only `nc`/`netcat` (4 links, `usr/bin` and the merged `bin`) were affected.

## Cause

`netcat-openbsd`'s maintainer script runs
`update-alternatives --install /bin/nc nc /bin/nc.openbsd ...`, which writes
`/bin/nc -> ../etc/alternatives/nc` -- correct from a real `/bin`. In the
prefix, `bin` is a **symlink to `usr/bin`** (merged-usr, base-files), so the
link physically lives in `usr/bin`, and `../etc` resolves to `usr/etc`, which
does not exist. `normalize-symlinks.sh` only rewrote **absolute** targets
(absolute symlinks that would escape the prefix), so it never touched an
already-relative link that the maintainer script had computed against the
logical `/bin`.

## Fix

`normalize-symlinks.sh` now also repairs a **dangling relative** link under
`usr/bin`/`usr/sbin`: if interpreting its target from the logical merged dir
(`/bin`, `/sbin`) lands on a real file, the link is rewritten relative to its
physical directory (`../../etc/alternatives/nc`). Verified: `nc` resolves and
runs (`OpenBSD netcat`).

The sweep otherwise passed for jq, tree, ncdu, zstd, unzip, curl (HTTPS,
HTTP/2), wget, git, sqlite3, tmux, nano, less, rsync, socat, lua5.4, nodejs,
python3 (+pip/venv), redis-server, imagemagick, and gcc + libc6-dev
(compile and run).
