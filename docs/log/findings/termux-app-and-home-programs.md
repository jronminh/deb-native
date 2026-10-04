# Findings: the Termux app's shell entry, and what runs in $HOME (2026-10-04)

<!-- template: templates/docs.template.md -->

**Impact: Isolated.** Investigation (no code change) behind the login /
userland-entry design (`TODO.md` 0.7.0): how the Termux app starts a session,
what app-level capabilities exist, and what a program needs to run from
`$HOME` with no prefix -- to judge whether a prefix-independent chooser or
shell in `$HOME` is feasible.

## Contents

- [The app's shell entry](#the-apps-shell-entry)
- [App capabilities](#app-capabilities)
- [What runs in $HOME with no prefix](#what-runs-in-home-with-no-prefix)
- [What this means for a $HOME chooser](#what-this-means-for-a-home-chooser)

## Related docs

- [`../../spec/userlands.md`](../../spec/userlands.md) -- one host, sibling
  userlands, and the interface between them.
- [`../../reference/android-platform.md`](../../reference/android-platform.md)
  -- Android/Termux platform facts.

## The app's shell entry

The Termux app starts a session by running `$PREFIX/bin/login`, which is a
`/bin/sh` **script**, not a binary:

- it prints `~/.termux/motd.sh` (or the static `/etc/motd`) when interactive;
- then `if [ -G ~/.termux/shell ]; then export SHELL="$(realpath
  ~/.termux/shell)"`, else it falls back to `$PREFIX/bin/bash`, `.../sh`, or
  `/system/bin/sh`;
- it exports `LD_PRELOAD=libtermux-exec-ld-preload.so` (termux-exec) if that
  file exists (with a `coreutils --coreutils-prog=true` probe), sources
  `/etc/termux-login.sh`, and finally `exec "$SHELL" -l "$@"` (interactive)
  or `exec "$SHELL" "$@"`.

So **`~/.termux/shell` is the session entry**, run as a login shell.
deb-native points it at `~/.dn-login` (a `/system/bin/sh` script), which then
`exec`s a chosen prefix's `dn-shell`.

## App capabilities

- **Config**: `~/.termux/termux.properties` (e.g. `allow-external-apps`,
  `default-working-directory`); reload with `termux-reload-settings`.
- **Android bridges**: ~90 `termux-*` clients in `$PREFIX/bin` (Bionic)
  talking to the Termux:API app -- battery, clipboard, notification, share,
  open/open-url, download, location, sensors, camera, sms, telephony, usb,
  wifi, infrared, nfc, fingerprint, keystore, dialog/toast, media
  (player/scan/microphone-record), tts/speech, volume/brightness/torch/
  vibrate/wallpaper, saf/storage, `termux-job-scheduler`, `wake-lock`,
  `termux-am` (Android intents), apps-info.
- **Storage**: `~/storage/*` symlinks into `/storage/emulated/0/...`
  (`termux-setup-storage`).
- `$HOME` = `/data/data/com.termux/files/home`; `$PREFIX` =
  `/data/data/com.termux/files/usr`.

## What runs in $HOME with no prefix

Direct probe (Termux clang; run under `env -i`):

- a `/system/bin/sh` script -- runs. (Our `~/.dn-login` is exactly this.)
- a **minimal Bionic ELF** (`clang -O2`): interpreter `/system/bin/linker64`,
  `NEEDED libc.so libdl.so` (Android system libs) -- runs with an empty
  environment, no prefix, no `PATH`.
- `clang -static` -- same, fully self-contained.
- **Not** prefix-independent: a **glibc** ELF (needs the prefix's fused loader
  and libs), or a Bionic program linking Termux-only libraries
  (`$PREFIX/lib`: `libc++_shared`, `libtermux-exec`, ...).

## What this means for a $HOME chooser

- A prefix-independent shell/chooser in `$HOME` **is feasible**: a
  `/system/bin/sh` script or a minimal Bionic ELF runs with zero prefix
  dependency, and can `exec` a chosen prefix's `dn-shell` (the kernel then
  loads that prefix's glibc). The current `~/.dn-login` is exactly this --
  which is why the login is already prefix-independent.
- A richer "display manager" (menu, manage prefixes, notifications) stays
  feasible at zero prefix dependency; any Android integration (`termux-*`) or
  TUI (ncurses) would need our own Bionic client/library rather than the
  prefix's -- deferred.
- The app runs **one** session entry (`~/.termux/shell`), so a chooser must be
  non-blocking and must always fall back to a shell.
