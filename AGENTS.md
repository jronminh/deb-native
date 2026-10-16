# AGENTS.md — deb-native

## What this repo is

The source of a runtime that runs an unmodified Debian `arm64` tree (a
**prefix**) on Android as an ordinary app, with no root, no `chroot` and no
namespaces, and the recipe for building a prefix in Termux. Nothing is shipped
prebuilt: users build the runtime and their prefix themselves
([`docs/building.md`](docs/building.md)). There is no CI.

- `src/dn-tiny/` — the runtime: `dn-tiny` (the tracer, no libc) and
  `dn-lane.so` (the shim, built in a prefix against its glibc).
  [`docs/architecture.md`](docs/architecture.md) is how they work.
- `scripts/build-runtime.sh` — builds `dn-tiny` (any aarch64 compiler; python3
  for the rows).
- `scripts/build-prefix.sh` — builds a prefix in Termux: debootstrap's first
  stage, the second stage under `dn-tiny`, apt, the shim, packages.
- `scripts/prefix/` — `boot.sh` (the entry) and `init.sh` (the tree's init),
  copied into a prefix's `boot/`.
- `docs/` — building, architecture, performance, spec (the model), reference.

The 0.7 runtime (`dn-monitor`, a PRoot fork, shipped prefixes, CI) is at tag
`legacy-dn-monitor`; nothing in the tree depends on it.

## Where it runs

Everything happens on the phone, in Termux: edit, build, test, commit, push
(`gh` is the git credential helper). The maintainer's phone is a Samsung Galaxy
S10e, Android 12, kernel 4.14; an Android 16 phone is the second target.

## Build and test

```sh
scripts/build-runtime.sh                     # build/dn-tiny
scripts/build-prefix.sh --set dev ~/work/p   # a fresh prefix, end to end
```

To test a runtime change without a new prefix, copy `build/dn-tiny` into an
existing prefix's `usr/lib/deb-native/`, and rebuild the shim in it with the
prefix's own gcc (`docs/building.md`, "Updating the runtime in a prefix"). Then
run what the change touches, with and without the shim, and compare against the
previous build interleaved: the phone's timings vary by 10-20% between runs.

Things worth running for any change to the tracer or the shim: `ls -lR` and
`find -printf` over `/usr` and `/etc` (diff the output against the previous
build), `stat`/`readlink`/`realpath` on links and missing names, `pwd -P`,
Python's `os` calls with `dir_fd`, `git init` and `git status`, `apt-get
install`, `useradd`/`userdel`, a threaded program, a `vfork` child that
`chdir`s, and a build (Lua 5.4 with `make`). `dn-tiny` without `-q` prints the
stop counts.

To test inside an app (dn-shell) rather than Termux: `run-as` runs in the
`runas_app` domain, not the app's, so type the command into the app's own shell
session (`adb shell input text ...`) and read its output from a file.

## Conventions

- Persisted text (code, docs, commits) in English.
- Docs state the current truth only; history lives in git.
- Comments say why, in the surrounding style; C is kernel style (tabs).
- Run one command at a time: no chaining of independent steps, no parallel jobs.
- Commit messages: a `part: what changed` subject, then why, and the measurements
  when the change is about speed.
