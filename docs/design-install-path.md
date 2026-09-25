# Install path: reuse Termux's apt/dpkg, don't fork them

Status: **architecture decision.** Not implemented yet.

## Decision

Do **not** fork or patch apt/dpkg source, unlike sudo-less. Use Termux's
own `apt`/`dpkg` binaries as-is, pointed at a separate prefix via their
existing relocation flags and a custom `apt.conf`.

## Why sudo-less had to fork, and why we don't

sudo-less runs on a real Debian host, where stock dpkg refuses to operate
without root ("requested operation requires superuser privilege"), calls
`chown`, and checks for `ldconfig` on `PATH` at startup. Their patches
(`0001-no-superuser-check`, `0002-no-chown`, `0003-no-ldconfig-check`)
remove exactly those checks — and per `docs/apt-dpkg-port.md` in sudo-less,
those patches are themselves lifted from Termux's own `termux-packages`
patch set, just made unconditional instead of `#ifndef __ANDROID__`.

We're running *in* Termux. Termux's apt/dpkg already ship the
`__ANDROID__`-guarded version of those same changes, built in. There is
nothing left to remove.

## What to do instead

### dpkg: relocate with existing flags, no chroot

```sh
dpkg --instdir="$NEWPREFIX" \
     --admindir="$NEWPREFIX/var/lib/dpkg" \
     --force-script-chrootless \
     --force-not-root \
     -i pkg.deb
```

This is stock dpkg functionality (`--instdir`, `--force-script-chrootless`,
`--force-not-root`), not a patch. It's also exactly what sudo-less itself
used *before* building "the view" (`apt-dpkg-port.md`, point 6: "Before the
view: `--instdir=$PREFIX` and `--force-script-chrootless`").

Consequence of skipping the view (accepted limitation, see
[`design-static-wrappers.md`](design-static-wrappers.md)): maintainer
scripts run via `--force-script-chrootless` execute directly with Termux's
own `/bin/sh`, seeing real absolute paths (`/etc/foo`) that do **not**
resolve into `$NEWPREFIX` — only `$DPKG_ROOT` (which dpkg exports to
scripts) tells a well-behaved script where the real files are. A script
that assumes `/etc/foo` unconditionally means the prefix will write to (or
fail to write to) the real root instead. This is the same class of gap
Direction 2 already flags, not a new one.

### apt: point Dir::* at the new prefix, no patch

A dedicated `apt.conf` (not committed to Termux's own):

```
Dir::State "NEWPREFIX/var/lib/apt";
Dir::State::status "NEWPREFIX/var/lib/dpkg/status";
Dir::Cache "NEWPREFIX/var/cache/apt";
Dir::Etc "NEWPREFIX/etc/apt";
RootDir "NEWPREFIX";
```

Loaded via `apt -o Dir::... ` overrides or `APT_CONFIG=path/to/this.conf`.
This mirrors sudo-less's generated `00local-prefix` — a config file, not a
source change.

`$NEWPREFIX` must be **separate from Termux's own `$PREFIX`**
(`/data/data/com.termux/files/usr`) — reusing it would let this project's
installs corrupt Termux's own package database. Use something like
`~/.local` (matching sudo-less's own default) or a dedicated directory.

## The two-layer database: seeding runs backwards here

sudo-less seeds the prefix's dpkg status from the **host's**
`/var/lib/dpkg/status` so apt treats already-present system libraries
(glibc included) as satisfied and only installs leaf packages. Their host
already has glibc — that's the whole premise.

Termux has no glibc at all. For glibc arm64 `.deb`s, we specifically *want*
apt to pull in the full `libc6` dependency chain into `$NEWPREFIX` — that's
the actual point of this project (glibc side-install, same rootfs
glibc-runner has been manually pointing patched ELF interpreters at).

Seeding may still be useful for the *other* direction: packages Termux
already provides a Bionic-native equivalent for (`zlib1g`, `libssl3`,
etc.), where duplicating a glibc copy into `$NEWPREFIX` would be wasted
disk/maintenance for no benefit if the plan ends up being "run glibc
binaries against the glibc side-install's own full lib chain" rather than
"mix and match Bionic and glibc libs" — mixing the two would be an ABI
minefield anyway, so the realistic default is: **don't seed from Termux's
own package set at all**; let `$NEWPREFIX` be a self-contained glibc tree,
and only reconsider seeding if disk footprint or duplicate maintenance
becomes an actual problem.

## Maintainer scripts calling root-only or missing helpers

Same caveat sudo-less documents (`apt-dpkg-port.md`, "Not patched, on
purpose"): scripts calling `ldconfig`, `update-alternatives`, `systemctl`,
`py3compile`, etc. need either a shim on `PATH` inside `$NEWPREFIX/bin` or
the package excluded. Not designed yet — first need a real sample of
maintainer scripts from target packages to see which helpers actually get
called (open item, same as the coverage survey in
`design-static-wrappers.md`).

## Open work

- [ ] Confirm Termux's shipped `apt`/`dpkg` versions support all the flags
      above (`--force-script-chrootless` in particular) — check
      `dpkg --version` / `man dpkg` on-device rather than assuming version
      parity with sudo-less's apt 3.3.3 / dpkg 1.23.11.
- [ ] Verify apt's signature verifier: sudo-less relies on the host's `sqv`
      (apt 3.x default); confirm what Termux's apt build uses (`sqv` or the
      older `gpgv`) and that it's present.
- [ ] Decide `$NEWPREFIX`'s location and whether it's user-configurable
      from day one or hardcoded for the R&D phase.
- [ ] Test `dpkg --instdir` + `--force-script-chrootless` against one real,
      simple glibc arm64 `.deb` (e.g. `hello`) end to end before attempting
      anything with maintainer scripts or dependencies.
