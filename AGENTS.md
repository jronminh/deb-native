# AGENTS.md — deb-native

## What this repo is

The source of a runtime that runs an unmodified Debian `arm64` tree (a
**prefix**) on Android -- in Termux, in a custom app, or over ADB -- with no
root, no `chroot` and no namespaces, and the recipe for building a prefix in
Termux.  The three hosts are [`docs/spec/hosts.md`](docs/spec/hosts.md). Nothing is shipped
prebuilt: users build the runtime and their prefix themselves
([`docs/building.md`](docs/building.md)). There is no CI.

- `src/dn-init/` — `dn-init`, the tracer (no libc): the tree's root process,
  `ptrace` + a seccomp filter, the exec gate, the law (`law.c`). Its `lane.c` is
  the source of `dn-lane.so`, the shim preloaded into dynamic glibc programs
  (`-DDN_LIBC`), which `scripts/runtime/build-shim.sh` builds.
- `src/dn-exec/` — `dn-exec`, the userland exec the gate hands every program
  to: maps the loader (or a static program's image) itself and patches its
  syscall sites at birth, before the jump.
  [`docs/architecture.md`](docs/architecture.md) is how the three fit
  together.
- `scripts/runtime/build-runtime.sh` — builds `dn-init` and `dn-exec` (any aarch64
  compiler; python3 for the rows).
- `scripts/runtime/build-shim.sh` — builds `dn-lane.so`: `--in-prefix PREFIX` with that
  prefix's own gcc, or `--cross` with `aarch64-linux-gnu-gcc`. The shim is a
  runtime artifact, so a prefix build only copies it; the ABI floor is whatever
  glibc it linked against, recorded beside the artifact.
- `scripts/prefix/build-prefix.sh` — builds a prefix in Termux: debootstrap's first
  stage, the second stage under `dn-init`, apt, packages. Nothing is compiled
  in the tree, so a minimal prefix needs no toolchain.
- `scripts/prefix/` — `boot.sh` (the entry) and `init.sh` (the tree's init),
  copied into a prefix's `boot/`.
- `docs/` — building, architecture, performance, spec (the model), reference.

The 0.7 runtime (`dn-monitor`, a PRoot fork, shipped prefixes, CI) is at tag
`legacy-dn-monitor`; the 0.9 runtime (`dn-tiny`, before the tracer/exec split)
is plain git history, nothing in the tree depends on either.

## Where it runs

The canonical workflow is on the phone, in Termux: edit, build, test, commit,
push (`gh` is the git credential helper).  A built prefix can run in Termux, a
custom app ([`docs/spec/host.md`](docs/spec/host.md)) or over ADB
([`docs/spec/adb-host.md`](docs/spec/adb-host.md)). The maintainer's phone is a Samsung Galaxy
S10e, Android 12, kernel 4.14; an Android 16 phone is the second target.

A second workflow edits from a Windows PC: WSL Debian carries
`gcc-aarch64-linux-gnu`, so the freestanding runtime binaries and the shim
cross-compile there, and the phone is driven over `adb`/SSH only for what must
actually run — see "The PC-over-adb workflow" below. What a prefix needs
beyond a compiler — debootstrap's second stage under `dn-init` — still happens
on the device.

## Build and test

```sh
scripts/runtime/build-runtime.sh                     # build/dn-init, build/dn-exec
scripts/runtime/build-shim.sh --cross                # build/dn-lane.so
scripts/prefix/build-prefix.sh --set dev ~/work/p   # a fresh prefix, end to end
```

To test a runtime change without a new prefix, copy `build/dn-init` (and
`build/dn-exec`) into an existing prefix's `usr/lib/deb-native/`, and rebuild
the shim with `scripts/runtime/build-shim.sh --in-prefix` over it (`docs/building.md`,
"Updating the
runtime in a prefix"). Then run what the change touches, with and without the
shim, and compare against the previous build interleaved: the phone's timings
vary by 10-20% between runs.

Things worth running for any change to the tracer or the shim: `ls -lR` and
`find -printf` over `/usr` and `/etc` (diff the output against the previous
build), `stat`/`readlink`/`realpath` on links and missing names, `pwd -P`,
Python's `os` calls with `dir_fd`, `git init` and `git status`, `apt-get
install`, `useradd`/`userdel`, a threaded program, a `vfork` child that
`chdir`s, and a build (Lua 5.4 with `make`). `dn-init` without `-q` prints the
stop counts.

To test inside an app (dn-shell) rather than Termux: `run-as` runs in the
`runas_app` domain, not the app's, so type the command into the app's own shell
session (`adb shell input text ...`) and read its output from a file.

## The PC-over-adb workflow

Editing in VSCodium on the Windows PC. The PC now cross-compiles what is
freestanding: `gcc-aarch64-linux-gnu` in WSL Debian builds `dn-init`,
`dn-exec` and, with `scripts/runtime/build-shim.sh --cross`, `dn-lane.so`. Anything
that has to *run* still happens on the phone -- a prefix's second stage, every
test suite -- driven from the PC over `adb`/SSH, through either of two hosts
([`docs/spec/hosts.md`](docs/spec/hosts.md)). Either way: `adb`, `git`, `gh`
are each added once to the PC's permanent user `PATH` (`C:\platform-tools`,
`C:\Program Files\Git\bin`) -- a fresh terminal picks them up without
re-exporting. (A cross-built shim carries an ABI floor from the glibc it linked
against -- WSL is trixie, so `GLIBC_2.38`; `build-shim.sh` records it in the
manifest and `--max-glibc` refuses a build above what the tree can load.)

- **The ADB host** ([`docs/spec/adb-host.md`](docs/spec/adb-host.md),
  implemented and tested): a prefix with no app at all, living under
  `/data/local/tmp`, entered with plain `adb shell` as `uid=2000(shell)` --
  no Termux, no SSH, and none of the broken-`PATH` problem below, since the
  shell's own environment is untouched by Termux's glibc tech preview. Once
  built, this is the simpler of the two for actually driving a prefix from
  the PC: `scripts/host/adb-shell.sh` (install once as `dn-adb`) or raw
  `adb shell 'cd /data/local/tmp/NAME && ./boot/boot.sh ...'`. Its one catch:
  `scripts/prefix/build-prefix.sh --host adb` still runs debootstrap's second stage on
  the device under the **app** runtime, and swaps in the adb `dn-init` and
  shim at the end -- both built with `--host adb`, since the shim has one
  host-dependent site -- so the phone is unavoidable for building, just not
  for running.
- **The Termux host**, entered over SSH instead of `adb shell` directly
  (Termux has no `adb shell`-reachable binaries of its own -- see the `PATH`
  note below). Covers both building and running, at the cost of the
  broken-`PATH` workaround.

Setting up the Termux host over SSH:

- The phone runs an `sshd` inside Termux (package `openssh`) on port 8022.
  `adb forward tcp:8022 tcp:8022` forwards it; this forward does not survive
  an `adb` server restart or a device replug, so re-run it when the phone
  drops. A dedicated ed25519 keypair (no passphrase) is in the PC's
  `~/.ssh/deb-native-termux`, its public half appended to Termux's
  `~/.ssh/authorized_keys`, so login is key-only. `~/.ssh/config` on the PC has
  a `dn-phone` alias (`HostName localhost`, `Port 8022`, `User u0_a323`,
  `IdentityFile ~/.ssh/deb-native-termux`) -- `ssh dn-phone '<command>'` runs
  something in Termux with no password prompt.
- **This phone's Termux has a second, broken "glibc" environment**
  (`/data/data/com.termux/files/usr/glibc/`) whose `lib/libc.so` is not a
  valid ELF, yet `PATH` lists `usr/glibc/bin` *before* the real `usr/bin`
  (bionic). Any ordinary coreutil resolved from `glibc/bin` (`mkdir`, `cat`,
  `env`, `wc`, `head`, ...) fails with "invalid ELF header" through this path;
  `sed`, as it happens, is not one of the shadowed names. Every command run
  over `ssh dn-phone` must therefore start with
  `export PATH=/data/data/com.termux/files/usr/bin;` first -- this is a
  pre-existing, unrelated breakage on this phone, not something to "fix" by
  reordering Termux's own `PATH` without asking.
- The real, worked-on clone is Termux's own `~/repos/deb-native` (origin
  `jronminh/deb-native`, same `main`) -- sshd's app-sandboxed filesystem
  means the PC cannot read or write it directly, only through `ssh`/`scp`.
  The PC's own clone is a second, independent checkout kept in sync by git
  (push from one side, pull on the other) or, for a file still uncommitted on
  the phone, by piping its content over `ssh ... cat PATH` into the PC's copy
  of the same path.
- The live test prefix is `/data/data/com.termux/files/p-dev` (Debian trixie,
  `set dev`, has a toolchain). A runtime change is tested by building it -- on
  the phone, or on the PC with `--cross` for the freestanding binaries and the
  shim -- and hand-patching the binaries into `p-dev` (`docs/building.md`,
  "Updating the runtime in a prefix"), not by building a fresh prefix each
  time. `p-dev/boot/version` is metadata
  written at debootstrap time; it does not auto-update when the runtime
  inside is hot-patched, so keep its `version`/`desc`/`ldhook` fields matching
  the tree's actual `VERSION` and binaries by hand after a patch, the way
  `built`/`size` stay pinned to the original debootstrap. Smaller throwaway
  prefixes (e.g. an alternate host or suite) are disposable -- delete them
  once done; `scripts/prefix/build-prefix.sh` remakes one in a few minutes.
- PowerShell quoting over this chain (PC shell -> `ssh.exe` -> Termux's
  `bash`) is fragile: nested double quotes and shell metacharacters
  (`(`, `)`, `|`) in one command have been silently dropped or
  mis-tokenized. Prefer single-quoted, metacharacter-free one-liners to
  `ssh dn-phone`; for anything with nested quoting, write a plain-text script
  file (no BOM, LF endings) and run it via `scp`, not by piping it over
  `ssh`'s stdin (observed to corrupt the first line).

## Conventions

- Persisted text (code, docs, commits) in English.
- Docs state the current truth only; history lives in git.
- Comments say why, in the surrounding style; C is kernel style (tabs).
- Run one command at a time: no chaining of independent steps, no parallel jobs.
- Commit messages: a `part: what changed` subject, then why, and the measurements
  when the change is about speed.
