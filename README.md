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
vs. sudo-less's 63%) — not because of anything fixed so far, but because
this project has never actually installed a package's ordinary
dependencies: `scripts/prototype-install.sh` only unpacks the one `.deb`
it's given. Wiring real `apt` dependency resolution (always the plan in
`docs/design-install-path.md`, never actually built) is now the clear #1
priority, ahead of everything else open in this repo.**

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
