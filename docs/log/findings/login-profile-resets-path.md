# Findings: the prefix login shell resets PATH (2026-10-04)

<!-- template: templates/docs.template.md -->

**Impact: Repo change.** `export PATH=...` inside a session "didn't stick", and
an adopted tool (`opencode`) and the host-layer commands (`dn-switch`,
`dn-list`, `dn-default`) were missing from a fresh session.

## Contents

- [Symptom](#symptom)
- [Cause](#cause)
- [Fix](#fix)

## Symptom

In a new userland session `command -v opencode` / `dn-switch` found nothing,
though both exist under `$HOME/.local/bin`. Adding
`export PATH=$HOME/.opencode/bin:$PATH` to `~/.bashrc` (what opencode's
installer does) changed nothing; `export PATH=...` in a session did not persist.

## Cause

Two things, both about which startup file a **login** shell reads:

- The app's session is a **login** shell (`dn-shell` -> `bash -l`, via
  `~/.termux/shell`). A login shell reads `/etc/profile`, `~/.bash_profile`,
  `~/.bash_login`, `~/.profile` -- **not** `~/.bashrc`. So the installer's
  `~/.bashrc` PATH edit is never read.
- Debian's `/etc/profile` (base-files) **overwrites PATH** wholesale:
  `PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin`. That
  discards the userland PATH `dn-launch.c` set, including the launcher dir
  (first, so per-binary launchers win) and `$HOME/.local/bin` (the host-layer
  commands and an adopted tool's wrapper). Verified: a login shell's PATH was
  exactly the Debian default, and `opencode`/`dn-switch` were absent.

(Programs under `$INSTDIR/usr/bin` still resolve because `/usr/bin` is shimmed,
which is why `htop` worked while `opencode` -- under `$HOME/.local/bin` -- did
not.)

## Fix

`make-shell-interface.sh` now generates `$INSTDIR/etc/profile.d/deb-native.sh`,
which `/etc/profile` sources **after** it resets PATH. It re-asserts the
userland dirs (launcher dir first) and `$HOME/.local/bin`. Verified: a login
shell now finds `opencode`, `dn-switch` and the launchers.

To persist a PATH addition in the prefix, put it in `~/.profile` (or a
`/etc/profile.d/*.sh` file), not `~/.bashrc`.
