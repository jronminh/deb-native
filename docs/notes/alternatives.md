# Compared with other ways onto Android

> Template: [`templates/docs.template.md`](../../templates/docs.template.md)
> (fix the relative path to match this file's depth). Read a doc's
> summary and table of contents below before its sections, and read
> its directory's own `README.md` first to confirm this is the right
> doc to open. Create a new doc, instead of extending an existing
> one, when the content is a distinct kind of writing -- a new spec
> topic, a new one-off investigation, or a new guide -- not just a
> long addition to what a doc already covers.

deb-native is not a container: nothing is isolated or emulated. Programs
are ordinary Termux processes that *see* a Debian layout. The project's
own method, and the five other ways people get Debian (or Debian-like)
software running on Android, side by side:

| method | how Debian's `/usr`, `/etc` appear | cost | package manager | root? |
|---|---|---|---|---|
| chroot (Linux Deploy) | a real `chroot` into a rootfs | none at runtime | Debian's own, in the rootfs | **yes** |
| proot-distro, UserLAnd, Andronix | a full rootfs image; every syscall of every process goes through proot (ptrace) | slow starts and file I/O, for everything | Debian's own, inside | no |
| Termux packages | not Debian: Termux's own ports (Bionic) | native | `pkg`, Termux's repo | no |
| glibc-runner (termux-pacman) | Termux's patched glibc; glibc binaries run by hand | native | no Debian packages | no |
| namespaces (Docker, Podman, sudo-less) | the kernel mounts the layout | native | Debian's own | needs user namespaces, which Android blocks |
| **deb-native** | the prefix is a real Debian tree; programs find it through the prefix's own glibc loader and an in-process path shim | native; the tracer only for static programs, raw syscalls, NSS | the prefix's own sources, database and base (Termux's apt/dpkg binaries) | no |

## Contents

- [What that buys](#what-that-buys)
- [What it costs](#what-it-costs)

## Related docs

- [`vs-sudo-less.md`](vs-sudo-less.md) — the structured, per-concern diff
  against sudo-less, the one entry in this doc's table solving the exact
  same problem on a different host.
- [`prior-art.md`](prior-art.md) — sudo-less and proroot in more depth,
  including what carries over and what's blocked on Android.
- [`design.md`](../spec/design.md) — why deb-native's own mechanism (the shim,
  the prefix's own loader, the tracer) works the way the table's last row says.

## What that buys

1. **No second system.** No distro image: `~/.dn` holds what you install
   plus a ~23-package base. Only glibc comes from Termux (the `libc6`
   stand-in); everything else is Debian's own `.deb`.
2. **No proot for normal programs.** proot-distro pays a ptrace round trip
   on every file access of every program. Here the shim rewrites paths
   inside the process, loaded by the prefix's own glibc loader when the program starts; in the
   0.2.0 survey every program that ran, ran this way.
3. **Mixed with Termux.** A Debian program is a Termux process: it calls
   Termux's programs and they call it, Termux's home is its `/root`, its
   launchers are on your `PATH`. There is no "logging in" to another
   system: you type `figlet`.
4. **Translated once, not emulated.** Each `.deb` is fixed at install
   (interpreter, library path, `#!` lines, maintainer scripts, hard links);
   afterwards it runs directly.
5. **Removable.** Termux is never modified: delete `~/.dn` and the
   `# deb-native` lines in `~/.bashrc`, and Termux is as before. (The
   opposite approach -- converting Termux itself, one way -- was tried and
   is dropped.)

## What it costs

- **A boundary, not everything.** Packages that need root, services and
  (for now) toolchains are out; a proot rootfs runs almost anything, slowly.
- **Not faithful Debian.** No real root, no init system; paths that bypass
  libc need care (the tracer, per-package fixes).
- **Not isolation.** A Debian program can touch your Termux files like any
  Termux program. (proot is no security boundary either.)

In one line: proot-distro puts a Debian machine next to Termux;
deb-native puts Debian's packages into it.

The one entry in the table that is also solving the *exact same problem*
on a different host, not a different problem on the same host, is
sudo-less (Debian-on-Debian, no root) — see
[`vs-sudo-less.md`](vs-sudo-less.md) for that side-by-side diff instead of
a table row.
