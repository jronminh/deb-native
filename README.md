# deb-native

**Install your favorite `linux-arm64` package through Termux's own `pkg`
workflow** — real Debian `.deb` (glibc) packages on Termux/Android, without
patching every binary by hand, and without the parts of the `sudo-less`
approach that Android's kernel/SELinux won't allow.

Status: **working prototype, 2026-09-25.** A real Debian arm64 `.deb`
(`hello`) installs and runs end to end via `scripts/prototype-install.sh`;
a real dependency (`ciso`'s `zlib1g`) resolves natively against Termux's
own glibc packages with zero files duplicated
([`docs/design-native-deps.md`](docs/design-native-deps.md)); and a real
hardcoded-path gap (`figlet`'s `/usr/share/figlet`, the exact case
sudo-less's kernel-level "view" exists for) is solved with a userspace
`LD_PRELOAD` shim instead, since neither the mount-namespace view nor FUSE
works on this device — confirmed with the actual syscall errors, not a
guess ([`docs/design-manual-overlay.md`](docs/design-manual-overlay.md)).
See [`docs/findings-prototype-2026-09-25.md`](docs/findings-prototype-2026-09-25.md)
for the first round's log and the still-open, still-unsafe workarounds
(architecture-name mismatch via `--force-architecture`).

**A random-sample survey (`docs/findings-survey-2026-09-25.md`), following
sudo-less's own methodology, found only 2 of 30 packages install (≈7%,
vs. sudo-less's 63%) — because this project had never actually installed
a package's ordinary dependencies: `scripts/prototype-install.sh` only
unpacks the one `.deb` it's given. Fixed and re-verified same day
(`docs/findings-survey-apt-2026-09-25.md`): `scripts/setup-apt-prefix.sh` +
`scripts/apt-install.sh` point real `apt` at a real Debian repo, scoped to
a separate prefix — same 30-package sample, same seed, now **10/30 (33%)**.
Two new failure classes found: maintainer scripts hitting hardcoded
absolute paths, and `dpkg` failing to recreate some packages'
intra-archive hard links on this filesystem. The first is now fixed —
see below.**

**Maintainer-script paths, solved (`docs/design-manual-overlay.md`):** a
`LD_PRELOAD`-based Bionic shim was built and tested, but turned out to be
chasing the wrong binary (`/bin/sh` on Android resolves to the OS's own
root-owned `/system/bin/sh`, not Termux's `dash` — and Termux's own shell
resists `LD_PRELOAD` interception too, being linked `BIND_NOW`). Abandoned
that dead end for something simpler and more robust: `scripts/patch-maintainer-scripts.sh`
rewrites a package's maintainer scripts as plain text (`sed`) between
`dpkg --unpack` and `--configure` — no interception, no linker, no
dependency on which shell is really running.

**Tested against a genuinely hard package** (`docs/findings-hard-package-2026-09-25.md`):
`ruby-adsf`, pulling in `ruby3.3`/`libruby3.3`/`debconf`/`ca-certificates`/
`openssl`. Found and fixed a real bug along the way — apt runs its own
`dpkg` in one combined invocation (unpack+configure internally, no
apt-level hook boundary between them), so the `DPkg::Pre-Invoke` hook
never fired at the right time; `apt-install.sh` now uses apt only to
resolve+download, then drives `dpkg --unpack` / patch / `--configure -a`
explicitly itself. `openssl` now configures correctly as a result. Hit a
real, deep, not-quick-to-fix wall one level further in:
`debconf`'s own `confmodule` (correctly found via the sed rewrite) calls
`/usr/lib/cdebconf/debconf` internally — a hardcoded path inside an
ordinary shipped file, not a maintainer script, so out of this mechanism's
current reach. Left as a named, understood gap (matches sudo-less's own
"study one by one" bucket for this exact class), not chased further —
install-side work now covers single-package leaf tools and their real
dependency chains that don't route through `debconf`/`cdebconf`.

**Update, same day (`docs/findings-dash-wrapper-2026-09-25.md`):** closed
most of that gap. Insight: dpkg just `execve()`s a maintainer script and
lets the *kernel* resolve its `#!/bin/sh` shebang — dpkg never chooses the
shell, so instead of patching dpkg or the real (root-owned, unpatchable)
`/system/bin/sh`, rewrite the shebang to point at a **real Debian `dash`,
installed through this project's own `apt-install.sh` and ELF-patched
with `grun`** — reusing infrastructure this repo already had. The
original glibc `path-redirect.c` shim (generalized to a wholesale
`/usr`/`etc`/`var`/`opt` mapping, plus `open64`/`stat64`/`access`/`execve`
support once `readelf --dyn-syms` showed `dash` needs the LFS variants)
now correctly redirects a real `dash` process. Found and fixed two sharp
edges along the way: `LD_PRELOAD` crashes `dpkg` itself outright if
exported before calling it (`dpkg` is Bionic; Bionic's linker won't start
with a glibc `.so` preloaded), and it leaks into a script's own forked
children (`cp`, etc.) unless unset right after the shell starts. `openssl`
and a plain `. /etc/foo.conf` case now work end to end through the real
pipeline.

Pushed further into `debconf`'s case (same doc): fixed two more real bugs
(`dash`'s `exec` builtin checks via `faccessat`, not `access`; `cdebconf`
is a separate package `debconf` doesn't strictly depend on — Debian
assumes it's already present, this project's from-scratch prefix has to
install it explicitly). That surfaced a **deeper, architectural wall**:
`cdebconf`'s own `preinst` (`mkdir -p /var/lib/cdebconf`) fails because a
package's `preinst` runs *during* `dpkg --unpack`, before
`patch-maintainer-scripts.sh` ever gets a chance to run — `preinst`
scripts are never patched at all, for any package. Fixing this for real
needs patching a `.deb`'s control scripts *inside the archive itself*,
before handing it to `dpkg` — a meaningfully bigger piece of work than
anything else done today.

**Built that, plus a proper base bootstrap
(`docs/findings-bootstrap-base-2026-09-25.md`):** `scripts/patch-deb.sh`
rewrites a `.deb`'s control scripts before `dpkg` ever unpacks it; a new
`scripts/bootstrap-base.sh` installs the packages real Debian assumes are
"always already there" (`base-files`, `base-passwd`, `dash`,
`debianutils`, `debconf`, `cdebconf`, `openssl`, `ca-certificates`) as one
transaction instead of piecemeal. Found and fixed three more real bugs:
batching `--unpack`-then-`--configure` breaks `Pre-Depends` ordering
(`base-files` needs `awk` *configured*, not just unpacked, before it can
even unpack) — now installs one package at a time, in apt's own resolved
order; a stale downloaded `.deb` from an earlier run gets silently
re-unpacked by a later one, undoing a `grun` patch — archives are now
cleared after every install; and patching the same script twice
(pre-unpack and the post-unpack safety net both touching it) doubles an
already-rewritten path — fixed with an idempotency marker. Most of the
base set now reaches fully configured; found, but explicitly **not yet
fixed**, why the rest doesn't: `update-alternatives` is already
`DPKG_ROOT`-aware (per sudo-less's own docs) and this project's blanket
path rewrite double-prefixes its arguments.

**Resolved, same day:** the `DPKG_ROOT` conflict turned out not to be an
`update-alternatives`-specific quirk — `base-files` itself is
`$DPKG_ROOT`-aware throughout, a real standard Debian convention more
scripts follow than just the dpkg-suite tools. Fix: **removed the static
path-rewrite entirely**, keeping only the shebang rewrite to the `dash`
wrapper — the runtime `LD_PRELOAD` shim already covers a script with no
`$DPKG_ROOT` awareness at all, by intercepting the actual syscall-adjacent
call with the literal path, and a `$DPKG_ROOT`-aware script's own
already-correct path never matches the shim's rewrite either, so nothing
double-applies from either direction.

That fix also exposed **the highest-value bug of the whole session**
(`docs/findings-sed-delimiter-bug-2026-09-25.md`): a `sed` command used
`#` as its delimiter while its own pattern started with a literal `#`
(matching a shebang's `#!`) — `sed: unknown option to 's'`, and under
`set -eu` this silently killed the entire patch script partway through
its file loop, every run, for every file alphabetically after whichever
one hit it first. This is exactly why `openssl` looked permanently broken
across many rounds of testing when the mechanism itself was fine. Found
only by invoking the script directly by hand and reading its real exit
code — a `|| true` one level up hid the crash completely inside the full
pipeline's logs. Fixed (switched delimiter to `,`): `openssl`, `dash`,
`debianutils`, and `mawk` all now reach fully configured.

Two new, genuinely distinct problems found and left open, neither a bug
in this project's own mechanism: **`chown` permission errors**
(`base-passwd`/`base-files` calling real `chown`, which no path redirect
can fix — the first real case for sudo-less's actual "shim" concept, a
no-op stand-in command, not path rewriting) and **external commands a
script forks aren't covered by the shim** (`readline-common`'s `cp`, a
separate Bionic process the wrapper deliberately clears `LD_PRELOAD`
before forking, to avoid crashing it — meaning that fork's own file access
isn't intercepted at all). Stopped here for this session (quota-conscious,
repeated direct instruction), with concrete next steps recorded.

## Why this exists

Termux ships its own package repo, rebuilt against Bionic (musl-like NDK
libc). A huge amount of the Debian archive never gets rebuilt for Termux.
Meanwhile a real Debian `.deb` for `arm64` is a normal glibc ELF binary —
Termux can already load glibc binaries one at a time via `glibc-runner`
(patch the ELF interpreter, run against a glibc side-install), but doing
that per-binary, by hand, does not scale to "apt-get install anything".

[`sudo-less`](https://github.com/jronminh/sudo-less) solves a related but
different problem: real `apt`+`dpkg` installing Debian packages into
`~/.local` **on a real Debian host**, no root, using a private mount
namespace that overlays the prefix onto `/usr /etc /var /opt` ("the view")
so a package's hardcoded absolute paths still resolve. Its docs are the
valuable part — see [`docs/prior-art.md`](docs/prior-art.md) for the full
breakdown of what carries over to Termux and what doesn't.

**The short version: about 40% of sudo-less's approach is reusable as-is.**
The other 60% — the mount-namespace + overlayfs "view", and the
`systemd --user` service layer — needs machinery this device doesn't have:
`unshare(CLONE_NEWUSER)` fails `EINVAL` (confirmed by strace — the kernel
itself has no unprivileged user namespace support here, not merely a
policy denial), and there's no systemd on Termux at all. The view's job is
now done instead by a userspace `LD_PRELOAD` shim (no kernel privilege
needed) — see [`docs/design-manual-overlay.md`](docs/design-manual-overlay.md).

## Approach

| sudo-less piece | on Termux |
|---|---|
| apt/dpkg forked to run root-less in a prefix, patches for no-superuser-check/chown/ldconfig-check | **not needed as a fork** — this *is* what Termux's own apt/dpkg patches already do, they're just built for Bionic. Reusable directly. |
| two-layer package db (host's `dpkg` status as read-only lower layer) | reusable idea: treat Termux's existing package set as the "already installed" layer, only fetch/install glibc leaf packages |
| `prefix-wrap` heuristics (does a binary need path-resolution help: absolute symlink out of prefix, missing interpreter, `ldd`-missing lib, hardcoded `/usr|/etc|/opt` path) | reusable as detection logic, independent of how the fix is applied |
| the "view": private mount ns + unprivileged overlayfs, live-patching path resolution at run time | **confirmed blocked** — `unshare(CLONE_NEWUSER)` fails `EINVAL` (kernel has no unprivileged userns support at all here, not just a policy denial), FUSE is also closed. Replaced by a userspace `LD_PRELOAD` path-redirect shim, verified working — see [`docs/design-manual-overlay.md`](docs/design-manual-overlay.md) |
| services via `systemd --user`, translated unit by unit | **doesn't exist on Termux** — being researched against `termux-services` (runit), see [Direction 3](docs/services-research.md) |

Install path decision (no fork/patch of apt/dpkg needed — see
[`docs/design-install-path.md`](docs/design-install-path.md)): Termux's own
apt/dpkg already carry the non-root patches sudo-less had to add for a real
Debian host, so this project reuses them as-is, relocated to a separate
prefix via dpkg's own `--instdir`/`--force-script-chrootless` flags and a
custom `apt.conf` — not a source fork.

Native dependency reuse (sudo-less's "native"/two-layer-db idea, adapted —
see [`docs/design-native-deps.md`](docs/design-native-deps.md), verified
working): a Debian dependency Termux's own glibc side-install
(`termux-pacman/glibc-packages`) already provides is left exactly where
Termux put it — no copy, found by the glibc dynamic linker's own default
search path — instead of sudo-less's approach of putting everything under
one `.local`. Only what's genuinely missing lands in this project's own
collection point, which plays `.local`'s role but only for the delta.

Same reuse-not-patch decision for triggering Direction 2's wrapper
generation — see [`docs/design-hooks.md`](docs/design-hooks.md): apt's
`DPkg::Post-Invoke` config hook plus a thin `dpkg` wrapper script on
`PATH` (for direct `dpkg -i` calls apt never sees), the same dual mechanism
sudo-less itself uses for `prefix-wrap`.

This repo is pursuing two directions in place of the blocked 60%:

1. **[Direction 2 — static per-binary wrappers](docs/design-static-wrappers.md).**
   Instead of a live mount-namespace overlay, generate a fixed wrapper script
   per binary at install time (same detection heuristics as `prefix-wrap`),
   pointing it at prefix paths directly (`--config`, env vars, or a
   glibc-runner-patched ELF interpreter) instead of making `/etc/foo.conf`
   resolve live. Less general, no namespace required, works everywhere
   Termux does.
2. **[Direction 3 — services on `termux-services` (research)](docs/services-research.md).**
   No `systemd --user` on Termux; `termux-services` (runit-based) is the
   native equivalent. Researching whether a package's systemd unit can be
   translated to a runit service script the way sudo-less translates it to
   a user unit.

## Non-goals

- Not a container, not a second distribution (that's `proot-distro`).
- Not trying to reproduce sudo-less's sandbox (`prefix-sandbox`,
  seccomp filters tied to the namespace) — that whole layer depends on the
  view, which isn't available here.
- Not for packages that need root at install or run time (system daemons
  with real system users, `setuid` binaries) — same caveat sudo-less states
  for its own prefix installs.

## Prior art / credit

- [`sudo-less`](https://github.com/jronminh/sudo-less) — the apt/dpkg
  prefix-install approach and its docs are the starting point for this
  repo's design. See [`docs/prior-art.md`](docs/prior-art.md) for the
  detailed carry-over analysis.
- Termux's own `apt`/`dpkg` patches (`termux/termux-packages`) — the
  original source sudo-less itself forked from; ends up being the piece
  this repo needs least modified, since it's already built for this exact
  environment.

## License

GPL-3.0-or-later (see [`LICENSE`](LICENSE)) — same license as `sudo-less`,
whose approach and docs this project builds on and adapts.
