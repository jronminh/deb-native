# TODO / roadmap

Current truth only. Finished work lives in the specs
([`docs/spec/`](docs/spec/)); this file is what is still open.

## Alpha goal (the next announcement)

**Goal**: pre-alpha means "works for the author"; alpha means "try it, it
mostly works". Reached when everything below is done. Announce with one
number ("N of 100 random Debian packages install and run, no root, no
proot"), the comparison with proot-distro (README), and a `gcc` demo.

**Status**: pre-alpha. A shipped prefix runs real Debian glibc packages
unprivileged; `apt install gcc` compiles and runs end to end.

**Open**:

- **Popular languages**: `python3` + a C-extension package, `perl` + an XS
  module, `ruby`, `nodejs` — the Perl version gap included (a package builds
  modules for one Perl ABI; the prefix's `perl` must match).
- **Unfiltered survey**: the seeded 100 across all sections without the size
  filter — a number for heavy packages, no asterisk.
- **More than one device**: a second phone / Android version, and the Google
  Play build of Termux.
- **Upgrade and remove**: `apt upgrade` across a Debian point release; an
  uninstall; Termux untouched afterwards.
- **Clear refusals**: an out-of-scope package says why ("needs a system
  service", "needs root") instead of a raw dpkg error.
- **Demo**: `apt install gcc`, compile, run — all inside Termux.
- **Auto-adopt downloaded glibc programs** (to consider): adopt on first run
  so a `curl ... | bash` installer lands with no manual `dn-adopt`. Adopt in
  place (keeps `/proc/self/exe`), announce on stderr, opt out
  `DN_NO_AUTO_ADOPT=1`.

## Prefix artifacts: core-ultra / core-deb, one ship path

Design: [`docs/spec/prefix-contract.md`](docs/spec/prefix-contract.md),
[`docs/spec/prefix-layers.md`](docs/spec/prefix-layers.md).

**Status**: the build applies the invariants and writes `.dn/`
(`scripts/build/package-prefix.py`), with the 256-byte `PT_INTERP` capacity
reserved by `dn-elf`; artifacts ship and bootstrap end to end
(`scripts/host/ship-prefix.sh`, `bootstrap-prefix.sh`).

**Open**:

- Produce the real `core-ultra` and `core-deb` artifacts and publish them.
- Build `core-ultra` from its own recipe instead of cutting it from a full
  prefix; `core-deb` = that recipe + the Debian layer.
- **Bootstrap on a poor host**: where the first artifact comes from, and how
  one artifact yields the next (core-ultra, core-deb, specialized).
- The `claude` specialized prefix: core-ultra recipe + the Claude binary as a
  glibc ELF at build time.
- `core-ultra` size: most `gconv` modules (~20 of 92 MB); terminfo missing.
- Launcher symlinks: make them relative after `normalize_symlinks` runs (the
  post hook's order leaves them absolute).
- **CI**: add a workflow that builds the prefix artifact
  (`build-overlay-glibc` → `build-core-deb` → `package-prefix`).
- **One build entry**: wrap the build steps in a single command (a `Makefile`
  or `scripts/build.sh`), so the README's build section is one line.

## Per-userland home (planned)

Give each prefix a **sparse `$HOME`** (`/data/data/com.termux/files/.dn/<name>/`)
for program state, while `/home` stays the user's data home, joined by **leaf**
symlinks — ends the two worlds' dotfile/config collisions without recursion.

## Services, then sudo (after alpha)

**Goal**: same scope as `sudo-less` — a service needs something to run it and
the rights it expects. In order: services, sudo modes, then both.

**Status**: not started. `sudo`/`doas` are pinned out; the no-op privilege
layer (`$DN/usr/lib/deb-native/priv/`) is a later real mode's replacement.
Design: supervisor is **runit** installed at deploy (Debian's `runit` package,
its `sysuser-helper` and init glue skipped, `runsvdir "$DN/etc/service"`
started by us). Packages keep shipping systemd units; we translate the unit at
install (foreground `ExecStart`, `Environment`, `WorkingDirectory`,
`RuntimeDirectory`/`StateDirectory`; `User=` and sandbox options dropped; low
ports/capabilities/devices refused with the reason) and the `systemctl` front
is a vendored, pruned SINS. Test ladder: `cron` -> `redis` -> `dbus`.

**Open**:

- `update-rc.d` / `invoke-rc.d` / `deb-systemd-helper` -> real runit
  translation (needs `/run` in the shim, system users in the prefix's db).
- Keep `dn-run`'s preload handling general enough for a second `LD_PRELOAD`
  (a fake-root library beside the shim).
- Keep `base-passwd`'s `root` user and `sudo` group as Debian has them.

## Shim & tracer hardening

**Status**: the libc-interposition layer is complete for its scope
([`docs/spec/shim/shim-coverage.md`](docs/spec/shim/shim-coverage.md)); NSS
and raw-syscall/static binaries route to `dn-trace`. `proot` is gone.

**Open**:

- Bake the shim into installed ELFs
  ([`docs/spec/shim/path-shim.md`](docs/spec/shim/path-shim.md), "Delivering
  the shim") so it survives an empty environment — `DT_AUDIT`/`--add-needed`,
  or `ld.so --preload`.
- Test `dn-run` -> `dn-trace` from an installed prefix, not just a checkout.
- **Post-install analysis pass** (not scheduled): name the public libc calls
  whose *internal* implementation probes a newer syscall and falls back on
  `ENOSYS` — fatal under Android's seccomp (`SIGSYS`). A per-launch check is
  rejected; the shape is a build/install-time analysis pass extending
  `scan-direct-syscalls.py`, then route only those binaries to the tracer.
- **Post-hook non-fatal faults**: a full bootstrap printed a few
  `Segmentation fault` lines from the post hook (it did not reproduce on a
  later run). Investigate.

## Runtime overhaul

`dn-elf` replaced `patchelf` everywhere ([`docs/reference/elf-interp-patch.md`](docs/reference/elf-interp-patch.md)).

**Open**: a name pass over the runtime — any name that no longer describes its
mechanism (`dn-run`'s "launch classifier", ...).

## Backlog (not yet scheduled)

- **Stale `apt.conf` hooks after a repo move**: the prefix's `apt.conf` bakes
  absolute checkout paths; nothing regenerates it for an existing prefix.
- **Run wrappers**: for each binary a package puts on `PATH`, generate a
  wrapper when it needs path help, triggered via apt's `DPkg::Post-Invoke`.
- **Classifier/refusal**: read each `.deb` before dpkg runs, classify scope
  and mechanism, refuse a "never" package before it wedges the prefix.
- **Soname-based dependency matching**: match a `.deb`'s `Depends:` against
  installed packages' SONAMEs directly.
- **Launcher/icon/desktop-DB integration**: only what the host needs.
- **State + `explain` + `doctor`**: record per package its scope, mechanism
  and wrappers, so decisions are explainable/removable.
- **Docs overlap**: `dn-glibc-prefix.md` and `dl-mechanics.md` both cover
  run-time prefix self-derivation; consider folding into one.

## Known unsafe, not yet fixed

- The `--force-architecture` workaround for the archive-name mismatch
  (`arm64` vs `aarch64`) needs a real fix.

## Blocked / impossible on this device

Kernel-wide (see [`docs/reference/android-platform.md`](docs/reference/android-platform.md)):
user namespaces off entirely, mount namespaces need `CAP_SYS_ADMIN`,
`/dev/fuse` root-only. Out of scope: install-view / service-view isolation,
seccomp+namespace sandboxing, other-architecture loaders and `Multi-Arch`
skew, setuid/setgid and file capabilities, a TUN device / firewall rules /
raw sockets.
