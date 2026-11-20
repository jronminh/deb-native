# AGENTS.md — deb-native

## What this repo is

The source of a runtime that runs an unmodified Debian `arm64` tree (a
**prefix**) on Android -- in Termux or over ADB, with no root, no `chroot` and no namespaces. It also
includes the Termux prefix builder and a custom-app host contract. The hosts
are [`docs/spec/hosts.md`](docs/spec/hosts.md). Nothing is shipped
prebuilt: users build the runtime and their prefix themselves
([`docs/building.md`](docs/building.md)). There is no CI.

- `src/` — the runtime's source, flat by choice: `dn-init` (the tracer, no libc:
  the tree's root process, `ptrace` + a seccomp filter, the exec gate, the law),
  `lane.c` (the shim `dn-lane.so`, built `-DDN_LIBC`), and `dn-exec.c` (the
  userland exec the gate hands every program to: maps the loader or a static
  program's image and patches its syscall sites at birth).
  [`docs/architecture.md`](docs/architecture.md) is how the three fit together;
  [`src/README.md`](src/README.md) is the file map.  No subdirectory per engine:
  the law, the rows and the lane cookie are shared, so splitting would only move
  include paths.
- `scripts/runtime/build-runtime.sh` — builds `dn-init` and `dn-exec` (AArch64 Clang
  and Python 3 for the policy rows).
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

## Where it runs

Build and test in Termux on an arm64 phone. The source also supports
cross-building the freestanding programs with AArch64 Clang and the
shim with a compatible glibc sysroot. Prefix bootstrap and device checks
still need Android. The [host overview](docs/spec/hosts.md) covers Termux,
ADB and the custom-app contract.

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

For a custom app, test from the app's own session. Android's `run-as`
uses a different SELinux domain, so its results do not establish what the
app itself can do.

See [the ADB host guide](docs/spec/adb-host.md) for deploying and testing
the shell-domain runtime from a PC.

## Conventions

- Persisted text (code, docs, commits) in English.
- Describe current behavior. Date historical measurements and record their source.
- Comments say why, in the surrounding style; C is kernel style (tabs).
- Run one command at a time: no chaining of independent steps, no parallel jobs.
- Commit messages: a `part: what changed` subject, then why, and the measurements
  when the change is about speed.
