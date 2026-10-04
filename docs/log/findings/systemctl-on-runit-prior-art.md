# Findings: systemctl-on-runit prior art -- SINS and friends (2026-10-04)

<!-- template: templates/docs.template.md -->

**Impact: Isolated.** Survey only (no code). Follow-up to
[`runit-for-the-prefix.md`](runit-for-the-prefix.md) /
[`runit-spike.md`](runit-spike.md): the `systemctl` front their "For our
prefix" / "Deploy" notes assume we would write -- does it already exist?

## Contents

- [SINS -- SINS Is Not Systemd](#sins----sins-is-not-systemd)
- [Debian / Devuan `systemctl`](#debian-devuan-systemctl)
- [Void, Artix](#void-artix)
- [What it means for us](#what-it-means-for-us)

## SINS -- SINS Is Not Systemd

[`github.com/Spinty-dev/SINS`](https://github.com/Spinty-dev/SINS) (Go, MIT,
young: ~19 commits). A real systemd-on-runit compatibility layer, not a toy:

- **`systemctl` shim**: `start stop restart reload status enable disable
  is-system-running daemon-reload show cat list-units list-unit-files mask
  unmask try-restart/reload-or-restart kill`; `--user` services under a
  private runit tree (`~/.runit/sv` -> `~/.runit/service`); targets
  (`multi-user.target`, `isolate`).
- **Unit translation**: `ExecStart`/`ExecStartPre` (shell-quoted), `Type=
  simple|notify|forking|oneshot`, `Environment`/`EnvironmentFile`,
  `WorkingDirectory`, `User=` -> `chpst -u`, `foo@.service` templates,
  `.socket`/`.timer`.
- **Optional modules** (Go build tags): a D-Bus bridge for
  `org.freedesktop.systemd1` + service activation, a notify socket, best-effort
  cgroups, a `.timer` scheduler, socket activation, and a `libsystemd.so(0)`
  shim + `sd_journal_*`/`sins-journalctl`.
- **Configurable for a prefix** via env (`pkg/runit/manager.go:26`,
  `pkg/systemctl/ctx.go:14`): `RUNIT_SV_DIR` (default `/etc/runit/sv`),
  `RUNIT_SERVICE_DIR`, `SYSTEMD_UNIT_PATH`, `SYSTEMCTL_PATH`; user mode hangs
  off `$HOME`. The `systemctl` core is pure Go -> a static `arm64` build runs
  on Android with no libc dependency.

Caveats for our context (Artix/Void desktop + root, not a single unrooted
Android user):

- `libsystemd.so` shim builds **x86_64 only**; the D-Bus/journal/notify stack
  is desktop-shaped and out of our scope.
- Generated `run` scripts **unconditionally** emit
  `mkdir -p /sys/fs/cgroup/sins/<name>` + `echo $$ > .../cgroup.procs`
  (hardcoded, no `|| true`) -- on Android's cgroup layout that errors (not
  fatal: the scripts do not `set -e`, and `/sys` is not shim-redirected).
  Modules can be left out, but those two lines live in the core generator.
- System masks go to `/etc/sins/masked` and notify to `/run/systemd/notify`
  -- both shim-redirectable, so fine inside a prefix.
- It **assumes `runsvdir` is already supervising** `RUNIT_SERVICE_DIR`; it does
  not own starting the supervisor in the right prefix. That is exactly
  [`runit-spike.md`](runit-spike.md)'s point #4 (`DN_INSTDIR` = that prefix) --
  so our own launcher is still needed regardless.

## Debian / Devuan `systemctl`

Debian ships an unrelated `systemctl` package (v1.4.4181, `Depends: python3`)
-- gdraheim's *systemctl3.py*, "daemonless `systemctl` without systemd". Its
backends are SysV/OpenRC; **runit is not first-class**, and it drags `python3`
into a prefix that deliberately keeps a small seed. Not a fit.

## Void, Artix

Neither ships an official `systemctl` shim -- both expect `sv`/`chpst`.
Community shims exist but SINS is the notable, actively-written one.

## What it means for us

- The hard part of the services layer -- parsing a unit into a `run` script
  and the `systemctl` command surface -- **has been written before**. SINS's
  `pkg/units` + `pkg/systemctl` + `pkg/runit` (Go, arm64) could be vendored as
  the translation engine, configured by env to point at `$DN`, with the
  D-Bus/journal/cgroup modules dropped.
- Counterweight: SINS targets a rooted desktop, so it carries cgroups/notify/
  `chpst -u` assumptions and core `run` lines we would patch, and it still
  needs our prefix-owned `runsvdir` launcher. The alternative -- keeping the
  plan in `TODO.md` ("Services, then sudo") of a small `systemctl` front
  mapping `start/stop/restart/status/enable/disable` to `sv` plus our own
  unit translation -- stays dependency-free and Android-shaped.
- Recorded, not decided: this entry exists so the choice is between a known
  vendorable engine and our own thin shim, not a guess.
