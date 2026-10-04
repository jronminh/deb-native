# Findings: vendored SINS `systemctl` -- forked, built, running (2026-10-04)

<!-- template: templates/docs.template.md -->

**Impact: Repo change.** Follow-through on
[`systemctl-on-runit-prior-art.md`](systemctl-on-runit-prior-art.md): rather
than write our own front, SINS was **vendored and pruned** to a minimal,
prefix-aware `systemctl`, built static for `arm64`, and exercised on the test
prefix. Code: [`../../../third_party/sins/`](../../../third_party/sins/README.md).

## Contents

- [What landed](#what-landed)
- [The patch series](#the-patch-series)
- [Verified on the prefix](#verified-on-the-prefix)
- [Why source-level redirect](#why-source-level-redirect)
- [Still open](#still-open)

## What landed

- `third_party/sins/upstream/` -- pristine SINS at commit
  `3d98fc0943669d59b4e7ebec901842857031e3f0` (MIT).
- `third_party/sins/patches/` -- our changes (below).
- `third_party/sins/build.sh` -- copies `upstream/`, applies `patches/`,
  prunes the desktop modules (`dbus`, `notify`, `cgroups`, `timers`,
  `sockets`, `libsystemd`, `sins-daemon`, `journalctl`, `systemd-analyze`,
  `supervisor`), and builds `CGO_ENABLED=0 GOOS=linux GOARCH=arm64` ->
  a 2.2 MB static `systemctl`.

## The patch series

- `0001-runit-prefix-aware.patch` -- `pkg/runit/manager.go` +
  `usermanager.go`: default `ServiceDir`/`EnableDir` derive from `DN_INSTDIR`
  (`$DN/etc/runit/sv`, `$DN/etc/service`) instead of `/etc/runit/sv` /
  `/var/service`; generated `run` and `log/run` shebangs point at
  `$DN/usr/bin/sh`; the two `/sys/fs/cgroup/sins/...` lines are dropped; the
  `Type=notify` socket becomes `$DN/run/systemd/notify`; the `cgroups`
  dependency is removed.
- `0002-systemctl-prefix-aware.patch` -- `pkg/systemctl/ctx.go`: the system
  mask dir and the unit search paths resolve under `DN_INSTDIR`.
- `0003-prune-go-mod.patch` -- drop the now-unused `godbus`/`x/sys`
  requirements so the build is pure stdlib and offline.

## Verified on the prefix

On `/data/data/com.termux/files/dn-070-test` (runit binaries + the spike's
`beat` service), with `runsvdir "$DN/etc/service"` running:

- `systemctl status|is-active|is-enabled|stop|start beat` all work
  (`status` reads `sv`'s live state; `stop`/`start` drive it).
- **Unit translation**: a fresh `hello-svc.service` was installed into
  `$DN/etc/systemd/system`, then `systemctl start hello-svc` generated
  `$DN/etc/sv/hello-svc/run` with a **`#!/data/.../dn-070-test/usr/bin/sh`
  shebang**, **no cgroup lines**, symlinked it into `$DN/etc/service`, and the
  service came up supervised, writing its log to `$DN/var/lib/hello.log`.

## Why source-level redirect

The `systemctl` binary is static Go, so the `LD_PRELOAD` shim never sees it --
It cannot be redirected the way Debian binaries are. That is the whole reason
to fork: the path logic lives in the Go source and reads `DN_INSTDIR`, so the
static binary writes into the prefix by construction. Everything it *spawns*
(`sv`, `runsv`, the services) is dynamic and still goes through the shim.

## Still open

- **Launcher**: still needs the per-prefix `runsvdir` starter with
  `DN_INSTDIR` set (exactly `runit-spike.md`'s point #4); the spike/this test
  start it by hand. Not wired into deploy yet.
- **Env conventions to fix**: the test used `RUNIT_SV_DIR=$DN/etc/sv` to match
  the spike's layout; the patched default is `$DN/etc/runit/sv`. Pick one
  layout before deploy.
- **`sv` needs `SVDIR`**: `sv` finds the enable dir via `SVDIR`, so the
  launcher/front must export `SVDIR=$DN/etc/service` for the spawned `sv`.
- Next experiments stay as planned: `cron` -> `redis` -> `dbus`.
