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

**Status**: shipped. `build-glibc.yml` builds the patched glibc from Debian's
own source; `build-prefix.yml` builds `core-ultra` and `core-deb` from it (an
empty base + pinned `scripts/build/packages.tsv` + `profile.txt`) and publishes
both to the single rolling `prefix` release. On device `ship-prefix.sh`
extracts, `.dn/install.sh` relocates (the artifact's loader runs its `dn-elf`),
and `.dn/bootstrap.sh` finishes core-deb (76 packages; `apt install figlet`
runs). The 256-byte `PT_INTERP` capacity is reserved by `dn-elf`.

**Open**:

- Build `core-ultra` from its own recipe instead of cutting it from the built
  core-deb tree; `core-deb` = that recipe + the Debian layer.
- **Bootstrap on a poor host**: where the first artifact comes from, and how
  one artifact yields the next (core-ultra, core-deb, specialized).
- The `claude` specialized prefix: core-ultra recipe + the Claude binary as a
  glibc ELF at build time.
- `core-ultra` size: most `gconv` modules; terminfo missing.
- Launcher symlinks: make them relative after `normalize_symlinks` runs (the
  post hook's order leaves them absolute).
- **`/etc/ca-certificates.conf` missing in core-deb**: `ca-certificates` is
  `ii` after bootstrap but the conf is absent, so `update-ca-certificates`
  adds 0 certificates and `/etc/ssl/certs/ca-certificates.crt` is never built;
  `git` over HTTPS fails with "Problem with the SSL CA cert". Workaround that
  worked: write the conf from `/usr/share/ca-certificates` (every `*.crt`,
  relative path, one per line), then run `update-ca-certificates`. The error
  `update-ca-certificates` prints (`sed: can't read /etc/ca-certificates.conf`)
  is the one in [`known-issues.md`](docs/reference/known-issues.md) ("Maintainer
  scripts with a raw interpreter shebang"); whether the bootstrap run of the
  `ca-certificates` postinst hit that same cause is not confirmed.
- **Account files**: `core-deb` has no `/etc/passwd`, `/etc/group` or
  `/etc/shells` although `base-passwd` is `ii`; the shim answers `getpwnam`
  with a synthesized entry whose shell is Termux's `login`. A login service
  (`dropbear`) rejects that shell. `core-deb` also lacks `libtalloc2`, which
  `dn-trace` needs.
- **Package list refresh**: `scripts/build/packages.tsv` pins exact versions;
  a mirror point release makes them unreachable. A resolver, or a documented
  refresh step, is needed.
- **One build entry**: wrap the build steps in a single command (a `Makefile`
  or `scripts/build.sh`), so the README's build section is one line.

## Runtime v1: new overlay (dn-policy / dn-glibc / dn-trace)

Design: [`docs/spec/runtime.md`](docs/spec/runtime.md). Replaces the whole
current overlay (`dn-shim.c`, `dn-run`'s adopt-on-first-run, `dn-elf` at run
time, `patch-maintainer-scripts.sh`) rather than running beside it. Phases
below match `runtime.md`'s roadmap; each phase must leave the tree runnable
on the current overlay until it's actually replaced.

**Status**: P1 mostly done, reusing `src/tracer/` (the pruned PRoot fork) as
`dn-trace`'s core rather than writing one fresh — answers the open question
below.

**P1 — exec gate, observe-only**:

- [x] `dn-trace` as the tree's root process needs no new boot-sequence code:
  `cli/dn-trace.c`'s existing `dn-trace [-v LEVEL] [-b HOST[:GUEST]]... --
  PROGRAM [ARG...]` already takes any script as `PROGRAM` (`dn-trace --
  any-init.sh`), and `launch_process()` (`tracee/event.c:139`) already
  installs the seccomp filter exactly once, which the kernel then carries
  across every `fork`/`exec` in the tree on its own — no "adopt per static
  binary" step needed once `dn-trace` is what starts the tree.
- [x] Exec gate classifies every `execve`/`execveat` by ELF header (the 5
  rules in `runtime.md`): `classify_exec()` in `execve/enter.c`, logged via
  `VERBOSE` only, changes nothing else yet. Verified by hand: a script, a
  static binary, a dynamic glibc binary, a `PT_INTERP` repointed to a
  foreign loader, and a plain non-ELF file each land in the rule their
  header says they should.
- **Not strictly minimal**: the filter installed today (`proot_sysnums` +
  `fakeroot_sysnums` in `syscall/seccomp.c`) already traces the path and
  identity groups too, not just `execve`/`execveat` — wider than P1's "exec
  rule only." Left as is rather than adding a toggle to shrink it, since
  nothing depends on it being minimal and it'll be superseded for real once
  dn-policy lands in P2/P3.
- Done when: `init.sh` and the existing services run under this `dn-trace`
  with no new errors, and a day's real use has a classification log.
  **Remaining**: the "real tree, real day" run — so far only verified by
  hand against one-off commands, not a booted prefix.

**P2 — the fast path**:

- [x] Define dn-policy's calling convention before either caller is written:
  `src/dn-policy/dn-policy.h` — function signatures, 0/-errno returns,
  caller-supplied buffers only (no library heap crossing the glibc/talloc
  allocator boundary, except the opaque `DnIdentity` handle, matched
  alloc/free), thread safety, and why `process_vm_readv`/`writev`
  marshalling is `dn-trace`'s own wrapper's job, not dn-policy's. Caught
  one real bug while writing it: fake uid/gid can't be a dn-policy global,
  since `dn-trace` tracks many tracees at once (one may drop to `_apt`
  while another stays root) — fixed with a `DnIdentity` handle the caller
  owns one of per traced program.
- [x] Implement path mapping: `src/dn-policy/dn-policy.c` —
  `dn_policy_init()`, longest-prefix mapping, `/proc`/`/sys`/`/dev`
  passthrough, no-double-translation, reverse translation. Verified by
  hand (`.check_translate.c`), caught and fixed a real bug (a spurious
  trailing slash translating guest `/`). **Not yet implemented:**
  absolute in-tree symlink resolution (correct for a tree with none,
  see the file's header comment).
- [x] Fake root: `src/dn-policy/dn-policy-fakeroot.c` — owner store
  (probed on-device: `user.dn.*` xattr works on the app's own data
  partition, so that's the default backend; the `RT/state/` DB fallback
  is implemented and reachable via `DN_POLICY_OWNER_BACKEND=db` for a
  device where it doesn't), `security.*` xattr faking, the
  `DnIdentity`-keyed get/setuid. Verified by hand against both backends
  (`.check_fakeroot.c`), including a slot-reuse case for the DB backend.
- [x] Hardlinks: `src/dn-policy/dn-policy-hardlink.c` — link2symlink, a
  refcount DB, and the stat/lstat fixup that makes a managed name report
  as an ordinary multiply-linked regular file. GPL question resolved (not
  actually a blocker, see `runtime.md`). Verified by hand
  (`.check_hardlink.c`).
- [x] Absolute in-tree symlink resolution: `map_and_resolve()`/
  `resolve_symlink_chain()` in `dn-policy.c`, with a `normalize_into()`
  cleanup pass for a relative symlink's `..`. Verified by hand
  (`.check_symlink.c`): absolute/relative/chained symlinks, a component
  appended past one, `_nofollow`, a not-yet-existing target, and
  `-ELOOP` on a self-reference. **dn-policy is now feature-complete**
  for everything `dn-policy.h` declares; not yet done: caching the
  resolution (every lookup hits the real filesystem directly).
- Patch glibc (full rebuild from Debian source, not the shipped 10-file
  swap — see `docs/spec/dn-glibc-prefix.md`'s status note): wire dn-policy
  into every path-taking function (public + internal + `syscall()`), route
  every kernel call through the gate page, map the gate page at loader
  startup, reorder library search (`RT/lib` first).
  - First slice done: `patches/dn-policy-glibc-wiring.patch` wires the public
    `open`/`openat` family (the eight `open*.c` call sites) to
    `dn_policy_redirect()`, rtld excepted via `#if !IS_IN (rtld)`. A full
    `make -O -j8` of glibc `2.41-12+deb13u4` with it applied links clean
    (verified 2026-10-08; see `patches/README.md`). Not yet in the build
    workflow — applied by hand in the scratch build.
- Pick `P_GATE`: inspect a few real on-device process memory maps for free
  39-bit space (open item in `runtime.md`).
- Turn on exec-gate rules 3/4 (rewrite to `RT/ld.so ...`). Drop the shim.
  Switch glibc-family packages to version pinning (`/etc/apt/preferences.d/dn-glibc`,
  auto-generated from `Source: glibc`), drop the hand-written resolver.
- Done when: packages in use, reinstalled from stock `.deb`s, run correctly;
  `apt update`/`install`/`upgrade` run end to end with no install-script
  failures.

**P3 — the fallback path**: turn on the path/identity syscall groups in the
shared filter; `dn-trace` handles them via `ptrace` through dn-policy,
replacing the current ptrace mechanism. **Must actually remove**
`src/tracer/syscall/exit.c`'s own `dn_fake_root()` (and the
`fakeroot_sysnums` list in `syscall/seccomp.c`) once dn-policy's fake root
is wired in — not leave both running side by side indefinitely, or
principle 3 ("one policy, two enforcement points, so they can never
disagree") is violated by the very code meant to honor it. Done when
`busybox-static`, a static Go program, and a Go-with-cgo program all run
correctly.

**P4 — measure and optimize**: per-program/per-syscall counters in
`dn-trace`; `process_vm_readv`/`writev`; path-resolution and exec-gate
classification caches. Done when there's a per-tier call-distribution table
for installing packages, compiling, and running a Python script.

**P5 — optional**: seccomp user notification for hot calls, only if P4's
numbers justify it.

## Per-userland home (planned)

Give each prefix a **sparse `$HOME`** (`/data/data/com.termux/files/.dn/<name>/`)
for program state, while `/home` stays the user's data home, joined by **leaf**
symlinks — ends the two worlds' dotfile/config collisions without recursion.

## Services, then sudo (after alpha)

**Goal**: same scope as `sudo-less` — a service needs something to run it and
the rights it expects. In order: services, sudo modes, then both.

**Status**: not wired into the prefix. `sudo`/`doas` are pinned out; the no-op
privilege layer (`$DN/usr/lib/deb-native/priv/`) is a later real mode's
replacement. Design: supervisor is **runit**, deployed as the programs
unpacked from Debian's `runit` `.deb` (the package itself pulls `adduser`,
`passwd` and PAM through `sysuser-helper`, and its init glue is skipped),
`runsvdir "$DN/etc/service"` started by us. Checked on a device: `runsvdir`,
`runsv`, `sv` and `chpst` run adopted, supervise a service, restart one that
exits, and take `sv down`/`sv up`. Packages keep shipping systemd units; we translate the unit at
install (foreground `ExecStart`, `Environment`, `WorkingDirectory`,
`RuntimeDirectory`/`StateDirectory`; `User=` and sandbox options dropped; low
ports/capabilities/devices refused with the reason) and the `systemctl` front
is a vendored, pruned SINS. Test ladder: `cron` -> `redis` -> `dbus`.

**Open**:

- **Deploy runit**: put the unpacked programs on the `PATH` of whatever starts
  `runsvdir` (it starts `runsv` by name; off `PATH` it logs `unable to start
  runsv`). Not yet checked: `svlogd` logging, `chpst -u`, `runit-init` and the
  `/etc/runit/{1,2,3}` stages, `runsvdir` surviving the end of the session
  that started it.
- **SSH service**: `dropbear` under `runsvdir` (OpenSSH's `sshd` cannot run:
  its privilege-separation `chroot` is blocked). `dropbear` logs in with a key
  today when started by hand; it needs a valid login shell in `/etc/passwd`
  and `/etc/shells` (see the account files item above), and `libtalloc2`.
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
