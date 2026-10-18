# Building and running a prefix

A **prefix** is a Debian `arm64` tree in a directory of your choice, with the
runtime inside it. You build it yourself, in Termux, from this repository: there
is no download of a prebuilt tree. How the runtime works is in
[`architecture.md`](architecture.md).

## Contents

- [What you need](#what-you-need)
- [1. Build the runtime](#1-build-the-runtime)
- [2. Build a prefix](#2-build-a-prefix)
- [3. Boot it](#3-boot-it)
- [The prefix](#the-prefix)
- [Moving a prefix](#moving-a-prefix)
- [Updating the runtime in a prefix](#updating-the-runtime-in-a-prefix)
- [Building by hand](#building-by-hand)
- [When something fails](#when-something-fails)

## What you need

- An `arm64` Android phone with Termux (from F-Droid or GitHub, not Google Play).
- In Termux: `pkg install git clang python debootstrap`.
- About 300 MB of space for a minimal prefix, 700 MB to 1 GB with a toolchain,
  and network access to a Debian mirror.

```sh
git clone https://github.com/jronminh/deb-native
cd deb-native
```

## 1. Build the runtime

```sh
scripts/build-runtime.sh            # -> build/dn-tiny
```

`dn-tiny` is one static executable with no libc; any compiler that emits
`aarch64` code builds it (Termux's `clang` is the default, `CC=gcc` works too).
`build/` also gets `dn-substtest` and `dn-census`, two test programs
([`reference/device-census.md`](reference/device-census.md)).
`build-prefix.sh` runs this step itself when `build/dn-tiny` is missing.

## 2. Build a prefix

```sh
scripts/build-prefix.sh ~/debian                 # minimal
scripts/build-prefix.sh --set dev ~/debian-dev   # with a toolchain, git, python3
```

What it does, in order:

1. `debootstrap --foreign`: fetches the base packages of the suite and unpacks
   them into the directory.
2. Puts the runtime in the tree: `usr/lib/deb-native/dn-tiny`, `boot/boot.sh`,
   `boot/init.sh`.
3. Runs `debootstrap --second-stage` under `dn-tiny`: every package's
   maintainer scripts run as they would on Debian, as root (fake root).
4. Writes the apt sources (the suite, its updates and security), runs
   `apt-get update`, installs `gcc` and `libc6-dev`, and compiles the shim,
   `usr/lib/deb-native/dn-lane.so`, inside the tree against its own glibc.
5. Installs the packages you chose, then `apt-get clean`, and writes
   `boot/version`.

On the S10e (Android 12), from a fresh clone: `minimal` in about 3 minutes
(265 MB), `dev` in about 3 minutes (811 MB), most of it the download.  The quick
start is the **Termux host**; the same tree also runs in a **custom app**
([`spec/host.md`](spec/host.md)) or over **ADB** ([`spec/adb-host.md`](spec/adb-host.md)),
and the overview is [`spec/hosts.md`](spec/hosts.md).

Options (`scripts/build-prefix.sh --help`):

| option | default | |
| --- | --- | --- |
| `--set minimal\|dev\|full` | `minimal` | a suggested package set (below) |
| `--packages "a b c"` | | any more packages |
| `--suite` | `trixie` | the Debian or Ubuntu suite |
| `--mirror` | `http://deb.debian.org/debian` | the Debian or Ubuntu mirror |
| `--distro` | from the mirror's name | `debian` or `ubuntu` |
| `--variant` | `minbase` | debootstrap's variant |
| `--host app\|adb` | `app` | `adb`: an ADB-hosted prefix (shell uid, no app) |
| `--runtime DIR` | `build/` | the app runtime used to build |
| `--runtime-adb DIR` | `build-adb/` | the adb runtime for `--host adb` |
| `--name` | the directory's name | the name in `boot/version` |
| `--no-shim` | | no `dn-lane.so`: everything goes through the tracer (slower) |
| `--ldhook` | | ship `dn-ldhook.so`, the loader hook (needs `execmod`/`execmem`) |
| `--cancel` | | patch glibc's shared cancellable `svc` (204 fewer stops in the Lua build, ~1% slower) |
| `--keep-toolchain` | | keep `gcc` and `libc6-dev` after the shim is built |

Ubuntu is the same command with its suite and mirror, detected from the mirror:

```sh
scripts/build-prefix.sh --set minimal --suite noble \
  --mirror http://ports.ubuntu.com/ubuntu-ports ~/ubuntu
```

The host has no Ubuntu keyring, so debootstrap's Release signature is not
checked there (`--no-check-sig`); the tree gets `ubuntu-keyring` and apt verifies
every package against it.

## An ADB-hosted prefix

No app: the prefix lives under `/data/local/tmp` and is driven from a PC over
`adb shell` (uid 2000, `Seccomp: 0`).  The prefix is **built** with the app
runtime (the second stage runs in Termux), and the shell runtime is put in place
at the end; the adb runtime has no app-seccomp answers.

```sh
scripts/build-runtime.sh --host adb build-adb
scripts/build-prefix.sh --host adb --runtime build --runtime-adb build-adb \
  --set minimal ~/p-adb
tar -C ~/p-adb -czf ~/p-adb.tar.gz .
scripts/adb-deploy.sh --port <wireless-port> ~/p-adb.tar.gz
```

`boot/version` says `host=adb`; `boot/binds` maps `/sdcard` (read-only); the
first boot reconciles the `user` entry to the shell uid.  For the shell itself,
install [`scripts/adb-shell.sh`](../scripts/adb-shell.sh)
as `dn-adb`: it refuses to enter without a connected adb device.  The model, the
limits and the measurements are in [`spec/adb-host.md`](spec/adb-host.md).

The sets are suggestions, nothing depends on them:

| set | packages |
| --- | --- |
| `minimal` | the debootstrap base and `apt` (the shim's `gcc` is removed again) |
| `dev` | `build-essential git curl ca-certificates python3 nano less procps file` |
| `full` | `dev` plus `openssh-client wget vim-tiny unzip xz-utils man-db htop` |

A prefix is an ordinary Debian system afterwards: `apt install` whatever you
need inside it.

## 3. Boot it

```sh
~/debian/boot/boot.sh                 # an interactive shell, as root
~/debian/boot/boot.sh apt install git # or any command
```

`boot.sh` starts `dn-tiny` with the prefix as its tree and runs `/boot/init.sh`
in it, which sets the environment (`PATH`, `HOME=/root`, `LANG=C.UTF-8`,
`TMPDIR=/tmp`) and runs the command.

`DN_TINY_FLAGS` passes options to `dn-tiny`. `boot.sh` polls 500 us for the next
stop by default (`-s 500`): faster when work spawns or stops a lot, a few percent
slower on a parallel build. `DN_TINY_FLAGS="-s 0"` sleeps between stops instead.

A prefix boots as the app's **real uid** by default: `id` is the app uid,
`set*id` and `chown` answer `EPERM`, and `/etc/passwd`, `group` and `shadow` get
its line at boot so `whoami`, `ssh` and `$HOME` work.  `DN_INITIAL=root
boot/boot.sh` seeds a root session for package management
(`apt`/`dpkg`/`useradd`); after that every syscall is classified by the current
euid -- there is no mode switch.  A setuid-bit helper can raise one process
(the gate reads the bit and the stored owner), and a sudo-like helper calls
`setgid(0)`/`setuid(0)` so the command is fully root.  Either way there is
**one real kernel uid**; other users are virtual (see
[`spec/identity.md`](spec/identity.md)).

## The prefix

```
DEST/
├── boot/
│   ├── boot.sh       the entry: run it, from anywhere
│   ├── init.sh       the tree's init (guest paths)
│   ├── binds         the host directories to bind (GUEST<TAB>HOST<TAB>FLAGS;
│   │                 optional; see docs/architecture.md, "The law")
│   ├── env           the host's variables: NAME keeps the host's value,
│   │                 NAME=VALUE sets it (optional; read by boot.sh, never
│   │                 sourced)
│   └── version       what the prefix is (key=value)
├── usr/lib/deb-native/
│   ├── dn-tiny       the tracer
│   ├── dn-lane.so    the shim (absent with --no-shim)
│   └── dn-ldhook.so  the loader hook (only with --ldhook)
├── usr/local/sbin/
│   ├── useradd       say that a user is virtual, then run the real one
│   └── adduser
├── var/lib/deb-native/
│   └── dn-tiny.gen   the shared link generation (made at the first boot)
└── ... a Debian tree
```

`boot/version`:

```
format=2
name=debian
desc=Debian trixie (minbase, set minimal) for deb-native 0.9.0-pre-alpha
version=0.9.0-pre-alpha
arch=aarch64
host=app
suite=trixie
shim=1
ldhook=0
cancel=0
built=2026-10-15T02:10:00Z
size=312
```

Inside the prefix the paths are Debian's: `/usr`, `/etc`, `/var`, `/root`,
`/tmp`. Only `/proc`, `/sys` and `/dev` are the phone's; `/data`, `/system`,
`/storage`, `/sdcard`, `/apex`, `/vendor` and `/mnt` are the tree's (a bind in
`boot/binds` is the way to reach one on purpose).

## Moving a prefix

A prefix names no absolute path of its own: `boot.sh` finds the tree from its own
location, so a prefix works wherever it is put.

```sh
tar -C ~ -czf /sdcard/debian.tar.gz debian       # in Termux
tar -C SOMEWHERE -xzf /sdcard/debian.tar.gz      # anywhere the user can run it
```

Another app (dn-shell, for instance) runs the same `boot/boot.sh`.

## Updating the runtime in a prefix

After pulling a newer deb-native:

```sh
scripts/build-runtime.sh
cp build/dn-tiny ~/debian/usr/lib/deb-native/dn-tiny
cp scripts/prefix/boot.sh scripts/prefix/init.sh ~/debian/boot/
cp src/dn-tiny/lane.c src/dn-tiny/lane.h src/dn-tiny/policy.c src/dn-tiny/policy.h ~/debian/tmp/
cp build/gen/policy-rows.inc ~/debian/tmp/
~/debian/boot/boot.sh gcc -O2 -shared -fPIC -DDN_LIBC \
  -o /usr/lib/deb-native/dn-lane.so /tmp/lane.c /tmp/policy.c
```

(the last two steps need `gcc` and `libc6-dev` in the prefix; the shim links the
law, `policy.c`, with the generated rows).

## Building by hand

`build-prefix.sh` is a convenience; this is all it relies on:

```sh
debootstrap --foreign --arch=arm64 --variant=minbase trixie DEST http://deb.debian.org/debian
# Termux's debootstrap empties useradd, chown and others in its second stage
# (for proot); drop those lines:
sed -i '/^[[:space:]]*echo "" > "\$TARGET\//d' DEST/debootstrap/debian-common
mkdir -p DEST/usr/lib/deb-native && cp build/dn-tiny DEST/usr/lib/deb-native/
cp scripts/prefix/boot.sh scripts/prefix/init.sh DEST/boot/
env -i HOME=/root PATH=/usr/sbin:/usr/bin:/sbin:/bin \
  DEST/usr/lib/deb-native/dn-tiny -t "$(cd DEST && pwd)" -- /bin/sh /debootstrap/debootstrap --second-stage
```

Any other way of putting a Debian `arm64` tree in a directory works as well (a
tarball of a Debian root, `mmdebstrap` on another machine): what makes it a
prefix is `usr/lib/deb-native/dn-tiny` and `boot/`.

## When something fails

- **The second stage fails**: `DEST/debootstrap/debootstrap.log` has the
  package and its script's output.
- **A program misbehaves only with the shim**: inside the prefix,
  `DN_LANE_PATCH=0 COMMAND` turns off the patching of libc; moving
  `usr/lib/deb-native/dn-lane.so` aside turns the shim off entirely.
- **`dn-tiny` prints a summary line**: drop `-q` from `boot.sh` (or run
  `usr/lib/deb-native/dn-tiny -t ROOT -- ...` yourself) for the counts of stops
  by kind; `-m bare` and `-c CPU` are for measurements.
