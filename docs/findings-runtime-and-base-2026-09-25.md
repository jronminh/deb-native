# Findings: a complete base bootstrap, and the runtime fix that unblocked it (2026-09-25, PM)

Outcome: on a **fresh** prefix, `scripts/setup-apt-prefix.sh` +
`scripts/bootstrap-base.sh` now bring the entire base set to
`Status: install ok installed` (`ii`) — `base-files`, `base-passwd`,
`dash`, `debianutils`, `debconf`, `cdebconf`, `openssl`,
`ca-certificates`, `mawk`, and their real dependency libraries (28
packages). Confirmed by `dpkg --admindir=.../var/lib/dpkg -l` and by a
functional check inside the prefix (`/etc/os-release` reports Debian 13
trixie; `dash`, `mawk`, `openssl` resolve to the prefix and run). Then
`scripts/apt-install.sh` installs a *new* package on top: `hello` lands and
prints `Hello, world!`.

This closes the two gaps
[`findings-bootstrap-base-2026-09-25.md`](findings-bootstrap-base-2026-09-25.md)
named as next steps (no-op `chown`; forked external commands need glibc
coverage) and the chicken-and-egg it left open for a fresh prefix.

## Root causes found and fixed (each by direct on-device testing)

1. **The maintainer-script interpreter was a shell script.** `dn-shell`
   was `#!/system/bin/sh` + settings. The kernel follows only **one** `#!`
   level, so `#!$INSTDIR/usr/bin/dn-shell` (where `dn-shell` is itself a
   script) is unexecutable; `dpkg` falls back to Android's Bionic
   `/bin/sh`, the glibc `LD_PRELOAD` never applies, and the run surfaces as
   the misleading
   `CANNOT LINK EXECUTABLE "/bin/sh": library "libc.so.6" not found`.
   **Fix:** `native/dn-launch.c` — a tiny **Bionic ELF** launcher (built
   with Termux's own `clang`) that derives `$INSTDIR` from
   `/proc/self/exe`, sets `LD_PRELOAD`/`DN_INSTDIR`/`PATH`/`DEBIAN_FRONTEND`,
   and `exec`s Termux's glibc `bash` (or, when invoked as `dn-perl`, glibc
   `perl`). `scripts/setup-runtime.sh` builds it and installs
   `dn-shell`/`dn-perl`.

2. **The chicken-and-egg (no Debian `dash` on a fresh prefix).** The old
   wrapper pointed at a Debian `dash` this project had to install first, so
   on a fresh bootstrap no wrapper could be created and `preinst` ran raw
   (cdebconf's `mkdir -p /var/lib/cdebconf` → `Read-only file system`).
   **Fix:** reuse Termux's **pre-existing** glibc userland
   (`$PREFIX/glibc/bin/bash` + `coreutils-glibc`), which exists before any
   Debian package is unpacked. This also makes commands a maintainer script
   *forks* glibc, so the shim reaches them — the gap the previous doc left
   open.

3. **`chdir` was not intercepted.** coreutils `mkdir -p` verifies an
   existing component with `chdir()`, not `stat()` (confirmed by strace:
   `mkdirat("$INSTDIR/var") = EEXIST` then `chdir("/var") = ENOENT`, the
   real `/var` being absent on Android), which coreutils reads as "not a
   directory". **Fix:** `chdir` interposer in `native/path-redirect.c`.
   Every `mkdir -p` on a path whose parent already exists was failing
   because of this.

4. **`grun --configure` rewrote this project's own runtime ELFs.**
   `apt-install.sh` grun-patches every ELF in the prefix; that corrupted
   the Bionic launcher and the shim (their interpreter became the glibc
   target, hence `libdl.so: cannot open shared object file`). **Fix:** the
   grun loop now skips `usr/bin/dn-shell`, `usr/bin/dn-perl`, and
   `usr/lib/deb-native/`.

5. **The shim's own home made `base-files` abort.** Placing it at
   `$INSTDIR/lib/deb-native` made `$DPKG_ROOT/lib` a directory, which
   `base-files`' `preinst` usrmerge check reads as "install usrmerge first"
   and refuses to unpack. **Fix:** moved the shim to
   `$INSTDIR/usr/lib/deb-native/`.

6. **`execvp` bypassed the shim.** glibc's `execvp`/`execvpe` walk `PATH`
   and call `__execve` *internally*, off the dynamic symbol table, so
   exporting `execve` alone never saw them. debconf's Perl frontend
   re-runs a package's config script with `execvp`; uncaught, the kernel
   resolved that script's shebang and handed Perl's glibc `LD_PRELOAD`
   straight to the Bionic interpreter. **Fix:** reimplemented
   `execvp`/`execvpe` (plus `execl`/`execlp`/`execle`) in the shim,
   funnelling through the same `do_exec` that strips `LD_PRELOAD` for
   non-glibc targets.

7. **Termux's `dpkg-realpath` is missing its data file.** It sources
   `$PREFIX/share/dpkg/sh/dpkg-error.sh`, which Termux's `dpkg` package
   does not ship. Pre-placed it (from Debian's `dpkg` package) at
   `$PREFIX/share/dpkg/sh/`; `dpkg-realpath` now runs clean.

Also added: no-op `chown`/`chgrp` shims on the wrapper `PATH`, and a much
wider intercept set in the shim (`statx`, `mkdirat`, `unlinkat`,
`symlinkat`, `linkat`, `renameat`/`renameat2`, `fchmodat`, `truncate`,
`utimensat`, `readlink`/`readlinkat`, `opendir`, `faccessat2`, `chdir`),
plus an `execve`/`execveat` dispatch that keeps `LD_PRELOAD` for glibc
targets and strips it for Bionic/scripts — which is what let the old
`unset LD_PRELOAD` hack be removed.

## Remaining, minor

- `dpkg-statoverride` is not shipped by Termux's `dpkg`; `ca-certificates`
  `postinst` logs `dpkg-statoverride: command not found` but still reaches
  `ii`. Either provide a no-op shim or pre-place the tool (it is a dpkg
  binary, so a shim is the lighter option).

## Files changed

- `native/dn-launch.c` (new), `native/path-redirect.c` (extended),
  `scripts/setup-runtime.sh` (new),
  `scripts/patch-deb.sh`, `scripts/patch-maintainer-scripts.sh`,
  `scripts/apt-install.sh`, `scripts/setup-apt-prefix.sh`.
