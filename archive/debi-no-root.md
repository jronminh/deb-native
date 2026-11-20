# Debi No Root reference

Debi No Root was an Android terminal APK proof of concept derived from
dn-shell. It treated the writable Debian tree as the app's userland and
kept terminal sessions and Android integration in the host.

## TTY behavior for a future Android host

- **Paste.** Strip ESC and C1 control characters from pasted text.
  Normalize LF and CRLF line breaks to CR. When the application enables
  DECSET 2004, surround the sanitized payload with the bracketed-paste
  start and end sequences. The tab menu exposed an explicit Paste action.
- **Bell.** Propagate BEL from the emulator through a session callback
  to the host. Debi used a tone generator and a user setting for the
  audible bell.
- **Title.** On first boot, install a Bash prompt hook if the guest
  user's startup file is absent. It sends the current directory as an
  OSC 2 terminal title, abbreviated to `~` at home. Preserve existing
  startup files.
- **Headless checks.** The device test ran the emulator without its
  View through Android's `app_process`. It covered paste sanitizing and
  bracketed paste, OSC titles, CSI cursor movement, BEL, resize and the
  alternate screen. The archived October 19 run recorded 11 passes.
  The live session already updated the PTY size before the emulator size.

These design lessons came from Termux's terminal work. The PoC exercised
them in its existing TermOnePlus-derived emulator.

## APK measurement

The 2026-10-18 proof of concept was installed and re-unpacked after wiping
app data on a Samsung Galaxy S10e. The recorded Debian trixie tree occupied
**355 MB** in `files/`. The separate `no_backup/` directory occupied
**84 KB** and held the historical `dn-tiny` tracer. The app refreshed the
tracer at each start and unpacked the tree only when the guest Bash was
missing. The launcher needed a system-shell `PATH` until handing control
to the guest, because picking the guest's `env` too early caused exit 126.

See the [custom-app host contract](../docs/spec/host.md),
[next-generation APK proposal](../docs/spec/dn-ng.md) and
[FOA recipe backlog](https://github.com/jronminh/fhs-on-android/blob/main/recipes/backlog.md).
