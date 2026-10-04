# deb-native

![status: pre-alpha](https://img.shields.io/badge/status-pre--alpha-orange)

**Install and run real Debian `arm64` `.deb` packages inside Termux — no root,
no `chroot`, no kernel namespaces.** `apt install PKG` works, and the program
runs by name.

- **No second system:** a small Debian tree (`~/.dn`), not a distro image;
  only glibc comes from Termux, every other package is Debian's own.
- **Native speed:** no proot for normal programs; paths are rewritten
  in-process, the tracer is only a fallback.
- **Part of Termux:** Debian programs are ordinary Termux processes, run by
  name, calling and called by Termux's own.
- **Removable:** Termux is never modified; delete `~/.dn` and it is gone.

How that differs from proot-distro, chroot and the rest:
[Compared with other ways](#compared-with-other-ways).

> [!WARNING]
> **Pre-alpha, AI-assisted, not independently audited.** Read `install.sh`
> and `scripts/` before running them. The on-disk layout can change between
> releases, heavy packages (toolchains) aren't supported yet, and this has
> had no security review. Use a throwaway Termux install or device.

## Requirements

Termux with `git`, `clang`, `patchelf`, and the glibc side-install
(`termux-pacman/glibc-packages`: `glibc-runner`, `coreutils-glibc`,
`bash-glibc`, `perl`, the loader and libraries). Everything else the project
needs (`apt`, `dpkg`, `dpkg-deb`) ships with Termux.

Optional: `make` and `libtalloc` (`pkg install make libtalloc`) to build the
syscall tracer, needed by static programs and ones making their own syscalls.
Without them those programs run untranslated.

```sh
# pinned pre-alpha release:
curl -fsSL https://raw.githubusercontent.com/jronminh/deb-native/v0.6.0+s.1-prealpha/install.sh | DEB_NATIVE_REF=v0.6.0+s.1-prealpha sh

# then restart Termux — the new session is the Debian userland
apt install figlet # the prefix's apt: Debian's packages
figlet hi          # an installed program, run by name
```

Or the rolling edge: replace both `v0.6.0+s.1-prealpha` occurrences with `main`.

![deb-native demo: installing Debian's lua5.4 inside Termux and running it](docs/demo.gif)

## Why this exists, and scope

The usual way to get Debian on Android is a **separate rootfs image under
proot** (or root): every syscall of every program through proot.
`deb-native` instead keeps a small Debian root of its own, `~/.dn`, and
runs its programs as ordinary Termux processes. Not an emulator and not
isolation: **install and run, not emulate**.

Scope is the same packages as [`sudo-less`](https://github.com/jronminh/sudo-less),
by Debian section ([`docs/spec/standard.md`](docs/spec/standard.md)): install
(reach `ii`) and run by name, unprivileged. Not yet: **services** (a package
that ships one installs, the service does not run; `runit` is the plan), and
packages that need root (system users, `setuid`, TUN, kernel modules; `sudo`
modes are planned). Toolchains: `apt install gcc`, a full compile
(`libc6-dev` included), and running the result all work now
(`TODO.md`).

Proof, not just a claim: 99 of 100 random Debian 13 packages installed and
ran, in a fresh prefix, with no tracer needed
([`docs/log/survey-0.2.0.md`](docs/log/survey-0.2.0.md)).

## Compared with other ways

deb-native is not a container: nothing is isolated or emulated. Programs
are ordinary Termux processes that *see* a Debian layout — unlike a
chroot (needs root), proot-distro/UserLAnd (a full rootfs, every syscall
through ptrace), or namespaces (Docker-style, blocked on Android). In one
line: proot-distro puts a Debian machine next to Termux; deb-native puts
Debian's packages into it. Full comparison table and trade-offs:
[`docs/spec/alternatives.md`](docs/spec/alternatives.md).

**The same idea as [sudo-less](https://github.com/jronminh/sudo-less), for
a platform that is not Debian** — same goal, same scope, same proof, the
mechanism inverted because the host and kernel are different. Full
side-by-side diff: [`docs/spec/vs-sudo-less.md`](docs/spec/vs-sudo-less.md).

## How it works

`install.sh` builds `~/.dn` debootstrap-style (Debian's index, `dpkg`/`apt`
-> Termux's binaries, Debian's real `libc6`/`libc-bin`, then this
project's own Android-patched glibc swapped in over them, then the Debian
base). An apt hook translates every `.deb` before dpkg sees it: its ELF
interpreter -> the prefix's own fused glibc loader, scripts and maintainer
scripts -> the prefix's shell. That loader is Debian's glibc source built
with this project's Android compatibility patches
([`third_party/glibc-android-patches/`](third_party/glibc-android-patches/));
it derives the live prefix from its own path at run time, loads the path
shim (`native/path-redirect.c`, via `$DN/etc/ld.so.preload`, rewrites
`/usr /etc /var /opt /root /lib /bin /sbin` into the prefix) and finds the
prefix's libraries from `$DN/usr/etc/ld.so.cache`. What the shim can't
reach (static binaries, raw syscalls, NSS) falls to `dn-trace`, a ptrace
tracer grown out of PRoot's core. Installed programs are linked into
`~/.dn/usr/lib/deb-native/bin`, first on `PATH`.

The deploy that installs Debian's package and swaps in the ten-file own
glibc build: [`docs/spec/deploy.md`](docs/spec/deploy.md) and
[`docs/spec/dn-glibc-prefix.md`](docs/spec/dn-glibc-prefix.md).

Full detail: [`docs/spec/design.md`](docs/spec/design.md) (the mechanism
end to end), [`docs/spec/install-flow.md`](docs/spec/install-flow.md)
(the bootstrap/install order), [`docs/spec/tracer.md`](docs/spec/tracer.md)
(the tracer).

## apt and dpkg

The default session is the **Debian userland**: `apt`/`apt-get`/`dpkg` (and
friends) are the prefix's, by PATH. Termux's own tooling lives in a host
shell — run `termux-shell` (green `~ $ ` prompt) for `pkg`, `termux-apt`,
`termux-dpkg`; `exit` returns. Inside the userland a `pkg` guard refuses.
Undo: `rm ~/.termux/shell` and delete `~/.dn`
([`docs/spec/host-userland.md`](docs/spec/host-userland.md)).

`install.sh` is idempotent — from a checkout: `sh install.sh [PREFIX]
[pkg ...]`; log in `~/.dn/var/log/`. `termux-dn-doctor` checks the common
breakages.

Two more commands (`dn-shell`, a shell inside the prefix; `dn-adopt`, to
run a glibc binary obtained outside apt through the prefix) and how root
works inside the prefix (fake identity, `DN_ID`, the tracer's cost):
[`docs/spec/design.md`](docs/spec/design.md) — "Day-to-day commands" and
"Fake root".

## Documentation

Full map in [`docs/README.md`](docs/README.md): specs (`docs/spec/` — the
design, package scope, shim/tracer/Android coverage, install flow),
engineering log and investigation history (`docs/log/`), and one-off
guides (`docs/guides/`). Start with
[`docs/spec/design.md`](docs/spec/design.md) for how the whole thing
works and [`TODO.md`](TODO.md) for current status.

- [`tracer/README.md`](tracer/README.md) — `dn-trace`, the ptrace tracer (from PRoot's core).
- [`TODO.md`](TODO.md) — roadmap · [`AGENTS.md`](AGENTS.md) — conventions.

## Credit & license

Built on other people's work — see [`CREDITS.md`](CREDITS.md):

- **[PRoot](https://github.com/termux/proot)** (`proot-me/PRoot`,
  GPL-2.0-or-later) — the `ptrace` syscall-interception core; `tracer/` is a
  reduced fork with its headers kept.
- **[talloc](https://www.samba.org)** (the Samba Project, LGPL-3.0-or-later)
  — PRoot's (and so `tracer/`'s) memory allocator; a build/run dependency,
  unchanged.
- **[Termux](https://github.com/termux/termux-packages)** — the Bionic host
  and the non-root `apt`/`dpkg` patches this project reuses as-is.
- **[`glibc-packages`](https://github.com/termux-pacman/glibc-packages)**
  (`termux-pacman`) — the glibc side-install every Debian glibc binary was
  repointed at before 0.5.0 (still the `install.sh` default); more directly,
  0.5.0's own-glibc patch
  ([`third_party/glibc-android-patches/`](third_party/glibc-android-patches/))
  is a **fork of this repo's own Android compatibility patches for glibc**
  itself, not written from scratch.
- **[Debian](https://www.debian.org)** — every installed package is
  Debian's own, unmodified beyond install-time translation; 0.5.0's `libc6`
  is Debian's real `glibc` source package repackaged with the patch above.
- **[sudo-less](https://github.com/jronminh/sudo-less)** — the prefix-install
  approach and the `apt`/`dpkg` lifecycle-hook idea.

Written with AI assistance (**Claude Opus 5.5**, **DeepSeek v4.1 Pro**).
GPL-3.0-or-later — see [`LICENSE`](LICENSE).
