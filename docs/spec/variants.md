# The variants: one runtime, many trees

The runtime is the core and stays Debian-agnostic ([`../architecture.md`](../architecture.md),
"The core and the variants"): a tree with a loader, its libraries and a program
is enough. A **variant** is a tree and its boot glue. This surveys the ones worth
building and what each costs, and it records what of the core still names Debian.

## The axes

- **Base**: `debian` (debootstrap) -- `busybox` (static) -- `bare` (one program
  and its closure).
- **Set**: `minimal` -- `dev` -- `full` -- `server` -- `compute`.
- **Options**: `--no-shim` -- `--ldhook` -- `--real-uid` -- a supervisor
  (`runit` or `busybox`).

## The variants

| variant | base | shim | size | use |
| --- | --- | --- | --- | --- |
| `bundle` | a program + its closure | static: no, glibc: yes | a few-tens MB | embed one tool |
| `busybox` | `busybox-static` | no | ~7 MB | 272 applets (shell, coreutils, sed/awk, tar, wget/nc/httpd/telnetd/udhcpc, crond, syslogd, init); could host services |
| `minimal` | debootstrap | yes | ~265 MB | a real Debian, `apt` |
| `dev` | + toolchain, git, python | yes | ~810 MB | develop on the phone |
| `full` | `dev` + extras | yes | ~1 GB | a CLI workstation |
| `server` | `minimal` + a supervisor | yes | ~300 MB | run daemons (`boot/services`) |
| `compute` | `dev` + Mesa Turnip | yes | ~400 MB | GPU offscreen (`/dev/kgsl-3d0`, `dma_heap`) |
| `gui` | `server` + Xwayland | yes | ~500 MB | a GUI; the host is the compositor |
| `other-distro` | debootstrap `--suite` | yes | ~300 MB | Ubuntu/Arch (the loader from `PT_INTERP`) |
| `musl` | alpine/minirootfs | a musl shim | ~50 MB | tiny, no glibc |

## Builder and image

`apt` is the heavy, networked part; it does not belong in a small image. Split
the roles: a **builder** (a full Debian prefix, or Termux + `debootstrap`) runs
`apt` and produces `.deb`s or a closure; the **image** consumes them, with
`dpkg-deb -x` (extract: no database, no `perl`) or `dpkg -i` (install: the
database, a shell and `perl`, but not `apt`). So the sizes step: **bundle (no
`dpkg`) -- mini (`dpkg`) -- minimal (`apt`)**.

## A worked example: the busybox bundle

A ~7 MB tree, made by `scripts/build-busybox.sh` from a `dev` builder:
`dn-tiny`, `busybox-static` (271 applet symlinks, so a tool that calls another by
name -- `tar` calls `gzip` -- finds it), `fastfetch` (glibc) and the loader with
`libc`/`libm`/`libyyjson`. It has no Debian userland and no `bash` -- its
`init.sh` runs under `busybox sh` -- and its own `boot/boot.sh` (no
`/usr/bin/bash`). It runs: `uname`, `ls`, a shell with globs and pipes, `tar
czf`, and `fastfetch`, which prints `OS: Debian GNU/Linux 13 (trixie)` from the
tree's `/etc/os-release` and the phone's real kernel, memory and uptime. It is an
appliance too: `busybox httpd` serves a file that `busybox wget` fetches back,
and -- once its `init.sh` writes `/etc/resolv.conf`, the tree ships none -- the
network works (`busybox wget` over HTTP).

## What still names Debian

The bundle shows the three places a tree can carry Debian; only the third is the
core's:

1. **The tree's own file.** `/etc/os-release` (`ID=debian`) -- by choice; a tree
   carries its identity and the runtime does not invent it (`phase2.md`).
2. **The builder's fingerprints.** glibc's banner (`Debian GLIBC 2.41-…`),
   `busybox`'s (`Debian 1:1.37.0-…`), `fastfetch`'s, and `dn-lane.so`'s
   `.comment` (`GCC: (Debian 14.2.0-19)`). Build from upstream (or clang, or
   musl) and they go.
3. **The core's one assumption.** `dn-tiny` has no `Debian` string, but it names
   Debian's multiarch path -- the loader
   `/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1` and the `--library-path`,
   in `policy.c`. This is the only Debian the core carries; it is open in
   [`../../TODO.md`](../../TODO.md), "Beyond Debian, and bundles".

The bundle is also the first test of that TODO item: it removed the *other* core
coupling (`boot.sh`'s `/usr/bin/bash`) by booting `busybox sh`. What is left is
the loader path -- fix it (take the loader from the program's `PT_INTERP`, let
the tree's `ld.so.cache` find the libraries) and the bundle names Debian nowhere
while the runtime is unchanged.
