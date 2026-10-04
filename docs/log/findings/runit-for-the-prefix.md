# Findings: runit in the prefix -- services, investigated (2026-10-04)

<!-- template: templates/docs.template.md -->

**Impact: Isolated.** Investigation (no code change) for the services layer
(`TODO.md`, "Services, then sudo"): what runit is, how Debian packages it, how
Termux uses it, and what it means to run a supervisor inside a deb-native
prefix -- unrooted, no init, glibc userland.

## Contents

- [What runit is](#what-runit-is)
- [How Debian packages it](#how-debian-packages-it)
- [How Termux uses it](#how-termux-uses-it)
- [For our prefix](#for-our-prefix)

## What runit is

A tiny process supervisor: `runsvdir DIR` watches every subdirectory of DIR;
each holds a `run` script that `runsv` starts, restarts if it dies, and
supervises; `sv up/down/restart/status` controls a service; `chpst` sets
per-service environment/limits; `svlogd` does logging. The binaries are small
(~68 KB each) and glibc-dynamic (interpreter `/lib/ld-linux-aarch64.so.1`),
so installed through the prefix's apt they are translated to the prefix's
fused loader and run as ordinary prefix programs.

## How Debian packages it

- `runit` (the binaries + `/etc/sv/*` definitions + the `/etc/runit/{1,2,3}`
  boot scripts) `Depends: runit-helper, sysuser-helper, libc6`.
- **`sysuser-helper` `Depends: perl, adduser, passwd`** -- the exact stack the
  minimal seed drops. runit's postinst (`dh_sysuser`) uses it to create the
  **`_runit-log`** user for `svlogd`.
- `dh_runit`'s postinst block runs `runit-helper postinst`, which enables
  `default-syslog` by symlinking into `/etc/runit/runsvdir/default`.
- `runit-run` / `runit-init`: the systemd/sysv and PID-1 integrations -- not
  applicable here.

## How Termux uses it

termux-services installs runit and supervises `$PREFIX/var/service`
(`SVDIR`), with service definitions shipped by packages (`sshd`, `ssh-agent`
are present there now). It is the same idea; the binaries live in Termux's
prefix (Bionic), so borrowing them would re-couple us to Termux's tree.

## For our prefix

- **Goal**: supervision inside the userland, owned by us -- not a PID-1 init,
  not systemd. runit fits: `runsvdir` is an ordinary process, and the service
  model is a directory of `run` scripts (deploy-as-file, matching a
  copyable/portable prefix).
- **Do not install Debian's `runit` package as-is**: its `sysuser-helper`
  (`adduser`+`passwd`+`perl`, ~60 MB) regresses the minimal seed, and its
  `_runit-log`/`runit-helper` glue assumes a system init. Instead use the
  **runit binaries** (from the `.deb`, translated) and run
  `runsvdir "$DN/etc/service"` ourselves, services as fake-root; no log user.
- **Layout**: `$DN/etc/sv/<name>/run`; enable = symlink into
  `$DN/etc/service`; `/run` and `/var/lib/<name>` already resolve in the
  prefix (R2).
- **Android**: the supervisor and services are ordinary processes -- Android
  may kill background work, so a wake-lock (`termux-wake-lock`, an app-level
  capability) is needed; there is no real init, so `runit`'s `/etc/runit/2`
  boot script is not used -- we start `runsvdir` directly.
- **Translation, not emulation**: a package's systemd unit maps to a `run`
  script (`ExecStart` foreground, `Environment[File]`, `WorkingDirectory`,
  `RuntimeDirectory`/`StateDirectory`); `User=`/sandboxing is dropped
  (fake-root) or the service is refused with the reason. A small `systemctl`
  shim maps `start/stop/restart/status/enable/disable` to `sv`.
- **Deferred**: `s6`/`dinit` if runit's dependency model proves too thin.
