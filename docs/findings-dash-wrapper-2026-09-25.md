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

## Follow-up, same session: two more real fixes, then a bigger wall

- **`faccessat`, not `access`.** `readelf --dyn-syms` on `dash` confirmed
  it: `dash`'s own `exec` builtin checks the target via `faccessat`
  (`UND faccessat@GLIBC_2.17`), not the plain `access` this shim already
  intercepted. Added.
- **`cdebconf` is a separate package `debconf` doesn't strictly `Depends:`
  on.** `apt-cache show debconf` has no hard dependency on it at all —
  real Debian assumes it's already present (it's `Priority: important`,
  part of the base install apt normally never has to think about). Our
  from-scratch prefix has nothing "already there," so it must be
  installed explicitly. `apt-cache show cdebconf` confirms it `Provides:
  debconf-2.0`, satisfying `ca-certificates`'s alternative dependency
  directly.
- Also added `DEBIAN_FRONTEND=noninteractive` to the wrapper (standard
  Debian practice for unattended installs, prevents a `debconf` prompt
  from hanging) and a second `--configure -a` pass in `apt-install.sh`
  (resolves ordering-only failures like `libruby3.3 depends on
  ruby-ruby2-keywords` after the package that provides it configures).

**New, deeper wall found installing `cdebconf` itself:**

```
mkdir: cannot create directory '/var': Read-only file system
dpkg: ... new cdebconf:arm64 package pre-installation script subprocess returned error exit status 1
```

`cdebconf`'s `preinst` (`cdebconfdir="/var/lib/cdebconf"; mkdir -p
$cdebconfdir`) hits the same hardcoded-path class of problem — but this
one is architecturally different from every case fixed so far. Checked
directly: the sed pattern *would* rewrite this fine (`="` followed by
`/var/` matches; the earlier worry that a variable-held path would be
invisible to sed was wrong for this actual case). The real problem is
**timing**: a package's `preinst` runs as part of `dpkg`'s `--unpack` step
itself, *before* `patch-maintainer-scripts.sh` ever gets to run (it runs
*after* `--unpack` in this pipeline) — so `preinst` scripts are never
patched at all, for any package, regardless of what they contain.

Fixing this needs a real architecture change, not a tweak: patch a
package's control scripts (and rewrite the shebang) **inside the `.deb`
file itself** — extract, patch, repackage — before ever handing it to
`dpkg`, instead of patching `$ADMINDIR/info/*` after the fact. That's a
meaningfully bigger piece of work (handling `md5sums` consistency, a
`dpkg-deb --build` round-trip per package) than anything else in this
doc. **Stopping here for this session** (quota-conscious): the debconf/
`cdebconf` chain is understood in real depth now, but not yet fully
closed. Next concrete step, in order: (1) build the pre-unpack `.deb`
patching pipeline, (2) retest `cdebconf` → `ca-certificates` →
`ruby-adsf` end to end.
