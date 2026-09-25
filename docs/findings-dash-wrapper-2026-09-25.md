# Findings: a real glibc dash as the maintainer-script "shell" (2026-09-25)

Follows directly from a user insight during design discussion: dpkg was
confirmed (by strace) to just `execve()` a maintainer script directly,
letting the *kernel* resolve its `#!/bin/sh` shebang — dpkg itself never
picks which shell runs. So instead of trying to patch dpkg, or intercept
the *real* `/bin/sh` (root-owned `/system/bin/sh` on Android, confirmed
unpatchable — `design-manual-overlay.md`), the shebang can simply be
rewritten to point at a shell this project fully controls: **a real
Debian `dash`, installed through this project's own `apt-install.sh`
pipeline, ELF-patched with `grun --configure` like any other glibc
binary** — closing the loop with infrastructure this repo already had,
rather than building a new Bionic-side mechanism.

## What's built and confirmed working in isolation

- `native/path-redirect.c` generalized from one hardcoded `FROM`/`TO`
  pair to a wholesale `/usr`, `/etc`, `/var`, `/opt` → `$DN_INSTDIR`
  mapping (same four directories sudo-less's view overlaid), plus
  `open64`/`stat64`/`lstat64`/`access`/`execv`/`execve` interceptors
  added alongside the original `open`/`openat`/`stat`/`fopen`/`symlink`.
- **Real Debian `dash`** (glibc, installed via `apt-install.sh dash`,
  patched via `grun --configure`) confirmed to need the `open64`/`stat64`
  family specifically — found via `readelf --dyn-syms`, which showed
  `dash` imports `open64@GLIBC_2.17`/`stat64@GLIBC_2.33`/etc., not the
  plain names the shim first only covered. Fixed and verified: `dash -c
  '. /etc/foo.conf'` correctly sources the redirected file and prints its
  content.
- `patch-maintainer-scripts.sh` now also generates a wrapper
  (`$INSTDIR/usr/bin/dn-dash`) and points each control script's shebang
  at it instead of `/bin/sh`.

## Two real bugs found and fixed while wiring this into the real pipeline

1. **`LD_PRELOAD` cannot be exported before calling `dpkg` itself.**
   `dpkg` is a Bionic process; Bionic's linker refuses to even start with
   a glibc `.so` in `LD_PRELOAD` (`CANNOT LINK EXECUTABLE ... library
   libc.so.6 not found`) — confirmed directly (`dpkg --version` under
   that env crashes outright). The wrapper script sets `LD_PRELOAD` only
   for the `dash` process it execs, never touching `dpkg`'s own
   environment.
2. **`LD_PRELOAD` then leaked into `dash`'s own children.** A maintainer
   script forking `cp` (a Bionic binary, found via `PATH`) crashed the
   same way, since environment variables propagate to forked children by
   default. Fixed by inserting `unset LD_PRELOAD 2>/dev/null || true` as
   the script's own second line (right after its rewritten shebang) —
   `dash`'s own interposition, already resolved at its process-load time,
   keeps working for the rest of its life regardless of the env var being
   unset afterward; only children forked *after* that point stop
   inheriting it.
3. **`PATH` needed the prefix's own `bin`/`sbin` first.** `debianutils`'s
   postinst calls `update-shells` by bare name; the wrapper now exports
   `PATH="$INSTDIR/usr/sbin:$INSTDIR/usr/bin:...:$PATH"`.
4. **Missing base skeleton directories** (`var/lib`, `var/log`,
   `var/cache`) — `setup-apt-prefix.sh` now precreates them; a package's
   own postinst assuming they exist (as they would on real Debian, seeded
   by `base-files`, which this project never installs) failed without
   them.

## Where it stands: real progress, not a finished mechanism

With all of the above, `openssl`'s postinst (`ln -s /etc/ssl /usr/lib/ssl`)
and a plain `. /etc/foo.conf` test both configure/run correctly through
the full real pipeline (`setup-apt-prefix.sh` → `apt-install.sh`).

`debconf`'s deeper case (the actual motivating hard problem —
`confmodule` calling `exec /usr/lib/cdebconf/debconf`) got **further than
before** — the error changed from `. /usr/share/debconf/confmodule: No
such file or directory` (couldn't even find confmodule) to `exec:
/usr/lib/cdebconf/debconf: not found` (found confmodule via the redirect,
reached the `exec`, but the exec target itself wasn't redirected) — but
is **not yet fully working**. Working hypothesis, not yet verified:
`dash`'s `exec` builtin likely checks the target's existence via
`access()`/`faccessat()` *before* calling the real `execve()`, and (same
pattern as `open`→`open64`) the actual symbol dash's `exec` implementation
calls might not be the plain `access` this shim already intercepts —
needs the same `readelf --dyn-syms` treatment `open`/`stat` already got.

**Stopping here for this session** (quota-conscious, per direct
instruction) rather than continuing to iterate. Next step is concrete and
narrow: check `dash`'s actual dynamic symbol imports for the access/exec
family the same way `open64`/`stat64` were found, add whichever variant
is missing, retest the `ruby-adsf`/`ca-certificates`/`debconf` chain from
`findings-hard-package-2026-09-25.md`.
