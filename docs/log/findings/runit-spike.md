# Findings: runit spike -- services run in a prefix (2026-10-04)

<!-- template: templates/docs.template.md -->

**Impact: Isolated.** Follow-up experiment to
[`runit-for-the-prefix.md`](runit-for-the-prefix.md) (no repo change):
installed the runit binaries into a test prefix and supervised a service end
to end.

## Contents

- [Setup](#setup)
- [Result](#result)
- [The four things to get right](#the-four-things-to-get-right)
- [What it means](#what-it-means)

## Setup

- Copied `runsvdir runsv sv chpst svlogd` from Debian's `runit` `.deb` into
  `$DN/usr/bin`, `patchelf --set-interpreter
  $DN/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1` on each (what an apt
  install does automatically).
- Service `$DN/etc/sv/beat/run` (`#!$DN/usr/bin/sh`, `PATH="$DN/usr/bin:..."`
  first, a loop appending the date to `/var/lib/beat.log`), enabled by
  `$DN/etc/service/beat -> ../sv/beat`.
- `runsvdir "$DN/etc/service"` started by hand; `sv status/up/down`.

## Result

- `sv status` -> `run: .../beat (pid ...)`; the heartbeat file filled in the
  target prefix (`Sun Oct 4 09:55:22/23/24 ...`); `sv down` stopped it. runit
  supervises correctly, all as ordinary processes -- no PID 1, no init.

## The four things to get right

1. **Interpreter**: the runit binaries are glibc (`/lib/ld-linux-aarch64.so.1`);
   translate them to the prefix's fused loader (`patchelf`, or apt install).
2. **PATH**: `runsvdir` execs `runsv` via `PATH` -- the prefix's `usr/bin` must
   be on it when the supervisor starts.
3. **`run` shebang**: point at `$DN/usr/bin/sh` (the prefix's dash), not
   `/bin/sh`, so the shim is loaded for the service.
4. **Run in the prefix's own environment**: the shim rewrites paths against
   `DN_INSTDIR`, so the supervisor and its services must be started with
   `DN_INSTDIR` = that prefix (through its `dn-shell`, or set explicitly).
   Started from a different prefix's session, the service's files silently land
   in the wrong prefix (observed: the log went to `deb-native` while the
   service belonged to `dn-070-test`).

## What it means

- The services layer is feasible and prefix-owned: runit binaries plus a
  service directory per prefix, started by us. Confirms
  [`runit-for-the-prefix.md`](runit-for-the-prefix.md); no unusual patching
  beyond those four points.
- Deploy: install the runit binaries into the prefix (skip the Debian
  package's `sysuser-helper`/init glue); a small launcher starts `runsvdir` for
  a prefix, with `DN_INSTDIR` set.
