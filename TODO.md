# TODO / roadmap

Ordered the way `sudo-less` orders its own work (`docs/design.md`, "Order
of work"), and judged by its criterion: **per-section coverage from a
random sample**, not feature count. See
[`docs/vs-sudo-less.md`](docs/vs-sudo-less.md) for the side-by-side diff
and [`docs/findings.md`](docs/findings.md)
for what just landed.

## Done recently

- [x] **Fresh base bootstrap to `ii`** (all 28 base packages) — see
      [`docs/findings.md`](docs/findings.md).
- [x] **Seamless launch**: `scripts/make-launchers.sh` + `scripts/dn-activate.sh`
      — installed programs run by name; `termux-exec` preserved (a Bionic
      child gets it back via `DN_BIONIC_PRELOAD`, a glibc child gets the shim).
- [x] **First full install script**: `install.sh PREFIX [pkg...]` — bootstrap
      if new, reuse if existing, install, generate launchers, activate PATH.
- [x] **Package scope decided + libc-shim coverage measured and completed**:
      [`docs/standard.md`](docs/standard.md) (section-based scope) and
      [`docs/shim-coverage.md`](docs/shim-coverage.md) (258-package in-scope
      corpus). Every imported path-taking symbol is now intercepted except the
      NSS lookups (untested), `glob`/`glob64` (indirect), admin ops, and the
      raw-`syscall()` boundary.

## Now / do first

- [ ] **Re-run the coverage survey on a fresh prefix** (sudo-less's
      yardstick; the base-env fix invalidates the old 33%).
      ```
      python3 scripts/sample-packages.py <packages-file>   # seed 20260925
      OUT=~/survey1 scripts/survey-apt.sh LIST.tsv
      ```
      Report: installed %, and the failure classes (unresolvable /
      maintainer-script / hardcoded-path / other). This decides whether
      the next build is run wrappers (#1) or the classifier (#2).

## Then, in order

- [ ] **Stage 4 run wrappers** (`prefix-wrap` equivalent) — the biggest
      unbuilt piece. For each binary a package puts on `PATH`, detect
      whether it needs path help (interpreter not present, interpreter's
      compiled-in module path, `ldd`-missing lib, hardcoded `/usr /etc
      /opt` path) and generate a wrapper. Build on
      [`docs/design.md`](docs/design.md);
      trigger via [`docs/design.md`](docs/design.md) (apt
      `DPkg::Post-Invoke` + a `dpkg` wrapper), not a source patch.
- [ ] **Stage 2 classifier / refusal** (`prefix-check` equivalent) — read
      each `.deb` before dpkg runs; classify scope (in / admin's / never)
      and mechanism (none / env / shim), flag unsafe maintainer scripts,
      and refuse a "never" package **before** dpkg can wedge the prefix.
- [ ] **Stage 1 sync on `apt update`** — keep the prefix's view of
      Termux's `*-glibc` packages fresh (`APT::Update::Post-Invoke-Success`),
      so a Termux upgrade doesn't leave stale seeds / "held broken
      packages".
- [ ] **Patch B: two-layer database, soname-based** — replace
      `scripts/native-seed.sh`'s hand-written ~10-entry name table with
      matching a `.deb`'s `Depends:` against installed `*-glibc` packages'
      `.so` SONAMEs (`docs/design.md`, "Open work"). Small,
      directly improves install success.
- [ ] **Stage 4 integration** — launchers/icons/desktop DB (mostly N/A on
      Android; do only what Termux needs).
- [ ] **Direction 3: services on `termux-services` (runit)** — translate a
      package's systemd unit into a runit `run` script
      (`docs/design.md`). The service *view* (config/state at
      `/etc/foo`, `/var/lib/foo`) is the hard part and shares Direction 2's
      unsolved gap.
- [ ] **State + `explain` + `doctor`** — record per package its scope,
      mechanism and the wrappers it got, so decisions are explainable and
      removable; `sudo-less` doesn't have these either.

## Quick wins

- [ ] no-op shim for `dpkg-statoverride` (Termux's `dpkg` doesn't ship it;
      `ca-certificates` logs `command not found` but still reaches `ii`).
- [ ] **`update-alternatives` writes links into Termux's own prefix**
      (`$PREFIX/usr/bin/awk -> $TERMUX_PREFIX/etc/alternatives/awk`), so
      alternative names are dangling. `make-launchers.sh` covers the
      `<name>-<pkg>` case (`figlet -> figlet-figlet`) but not `awk -> mawk`.
      Either fix the link target or resolve providers from the alternatives db.
- [ ] **forked Bionic `sed`/`find`** in some postinsts can't see prefix paths
      (`ca-certificates` logs `sed: can't read /etc/ca-certificates.conf` but
      still reaches `ii`). The shim only covers the glibc shell's own calls;
      install Debian `sed`/`findutils` or wrap them.
- [ ] refresh `README.md` status numbers once the survey (#Now) reports.

## Shim hardening (from code review + platform probes, 2026-09-26)

Tracked in [#1](https://github.com/jronminh/deb-native/issues/1).

- [x] **Fix `unlink()`** — fixed in `521cc73` (was declared with
      `unlinkat_t` and called `real(AT_FDCWD, path, 0)`, passing `(char
      *)-100`).
- [x] **Grow the intercepted libc surface** — the issue-#1 gaps landed in
      `521cc73` (`lstat`; `fopen64`/`freopen64`/`openat64`/`fstatat64`/
      `truncate64`; fortified `__open_2`/`__openat_2`/`__open64_2`;
      `statfs`/`statvfs`; `dlopen`/`dlmopen`; AF_UNIX `bind`/`connect`).
      This change finishes the libc layer with the rest of the path-taking
      surface: `creat`/`creat64`/`freopen`; `chown`/`lchown`/`fchownat`;
      `utime`; the xattr family (`setxattr`/`lsetxattr`/`getxattr`/
      `lgetxattr`/`listxattr`/`llistxattr`/`removexattr`/`lremovexattr`);
      `mkfifo`/`mkfifoat`/`mknod`/`mknodat`; `statfs64`/`statvfs64`;
      `realpath`/`canonicalize_file_name`; `inotify_add_watch`; AF_UNIX
      `sendto`; `mkstemp`/`mkostemp`/`mkdtemp`; `posix_spawn`/
      `posix_spawnp`. Verified on-device by `tests/shim-libc/run.sh`
      (asserts each symbol rewrites; `posix_spawn` of a redirected glibc
      binary runs). What this layer **cannot** reach is the tracer's job:
      raw `syscall()`, static binaries, and libc-internal opens that bypass
      the PLT.
- [ ] **Bake the shim into installed ELFs** (see `docs/design.md`,
      "Delivering the shim"): `patchelf --add-needed`/`--add-rpath`, or a
      `DT_AUDIT` module, so the shim survives an empty environment; the
      explicit loader (`ld.so --preload`) is the simpler variant.
- [ ] **Syscall-level tracer** for what libc interposition can't see (static
      binaries, raw `syscall()`, libc-internal `dlopen`/NSS): `ptrace` or
      `SECCOMP_RET_USER_NOTIF`, inside the app uid. Feasible — `ptrace` works
      here (`proot` runs); namespaces/overlayfs/FUSE do not.

## Open, still-unsafe

- [ ] `--force-architecture` workaround for the archive-name mismatch
      (`arm64` vs `aarch64`) — flagged unsafe in
      `docs/findings.md`; decide on a real fix.

## Blocked / impossible on this device

Keep these out of scope. Probed 2026-09-26 (`docs/findings.md`, "Platform
sandbox limits"): user namespaces are off **kernel-wide** (`CLONE_NEWUSER` =
`EINVAL` even from the seccomp-free shell), mount namespaces need
`CAP_SYS_ADMIN`, and `/dev/fuse` is root-only:

- install-view / service-view isolation (hidden `$HOME`, empty `/run`);
- `prefix-sandbox`'s seccomp + namespace isolation;
- other-architecture (i386) loaders and `Multi-Arch` skew;
- setuid/setgid and file capabilities;
- a TUN device, firewall rules, raw sockets.
