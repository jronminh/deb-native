# AGENTS.md — deb-native

## What this repo is

`deb-native` builds a **self-contained Debian `arm64` userland in a tarball**
(a "prefix"): its own glibc, `apt`, and files. A **poor host** installs it
with nothing but a POSIX shell and `tar`/`dd`/`sed` (toybox is enough) — no
root, no `chroot`, no namespaces, and the host's own tree untouched. Goal: a
package reaches `dpkg` status **`ii`** and its program runs **by name**,
unprivileged. "Install and run, not emulate" — no isolation.

## How it ships: build → ship → boot

There is **no build on the host**.

- **Build** (a build host: `scripts/build/`): the runtime
  runtime (`scripts/build/build-trace.sh` — `dn-trace`, one static
  binary with the syscall catalog embedded, plain-gcc, linked against stock
  `libc6-dev`), and the prefix artifact
  (`build-core-deb.sh` + `package-prefix.sh`, which writes `boot/` and
  `var/lib/deb-native/` —
  `docs/spec/prefix.md`).
- **Ship** (`scripts/host/ship-prefix.sh`, the host's own shell): read
  `boot/version` without extracting, extract, run `boot/install.sh`. This is the
  one install path, on a **poor host** (POSIX sh + toybox only).
- **Activate / boot**: the artifact's own `boot/install.sh` (host shell) wires
  the session entry; the host then boots the tree by running the entry,
  `boot/boot.sh`, which starts `dn-monitor` and the prefix's init -- `dn-monitor`
  derives the prefix root from its own location, so the artifact is relocatable;
  init completes `var/lib/deb-native/profile` itself on a first boot.

Run time is one policy in two places: dn-policy, called in-process by
dn-shim (the fast path) and via `dn-trace`'s ptrace fallback
(`src/tracer/`). The old shim, maintainer-script rewriting and ELF editor
are gone; a `.deb` installs intact (`docs/spec/runtime.md`).

## Layout

- `src/` — `dn-policy/` (the shared policy library), `tracer/` (dn-trace, a
  pruned PRoot fork), `dn-tiny/` (a freestanding tracer with no PRoot code,
  not yet wired into the runtime: `docs/reference/ptrace-interception.md`),
  `syscalls.tsv` (the syscall catalog).
- `scripts/build/` — build host: runtime, artifact assembly, packaging.
- `scripts/host/` — the host's own shell (poor host): ship,
  install (activation), update.
- `scripts/prefix/` — run by the prefix (its own glibc); `interface.tsv` is
  the module surface.
- `patches/` — the glibc patches: the Android base + the dn-policy wiring.
- `tests/` — on-device smoke tests; `tools/` — repo checks + helpers;
  `docs/` — spec, reference, notes.

## Where truth lives / workflow

- **Source of truth is the phone: `ssh fe2` → `~/deb-native`** (the build host
  with the toolchain). It pushes to GitHub. A local copy may be stale: edit
  locally, `scp`/`tar` to `fe2`, build and test there, then commit and push
  there. `gh` is authed as `jronminh`.
- **Current WSL workspace:** `/root/deb-native` is the current clone used for
  editing and cross-building. `/root/deb-native-old` is an archived older
  clone; do not use it for current runtime work.
- **Device test workflow:** build the AArch64 runtime or prefix artifact in
  WSL, push test files with `adb` to `/data/local/tmp`, then copy them into
  the debuggable `org.dn.shell` sandbox with `run-as`. The active device is
  the Samsung `SM-G970W` (Android 12, SDK 31, arm64-v8a); check the actual
  connection with `adb devices -l` rather than assuming a port or IP.
- **App test environment:** `org.dn.shell` has `targetSdk=28` and supports
  `run-as`. The current runtime prefix is
  `/data/user/0/org.dn.shell/files/prefixes/dn-stock`. It contains the
  `0.7.1-dev` runtime with `dn-monitor`, `dn-interp`, and `dn-shim.so`.
  `core-deb` and `dn-gcc` on the device are legacy prefixes using the older
  runtime and must not be used for current runtime experiments.
- **Direct device test:** use absolute paths and invoke the prefix entry
  directly, for example:

  ```sh
  adb shell run-as org.dn.shell \
    /data/user/0/org.dn.shell/files/prefixes/dn-stock/boot/boot.sh \
    /usr/bin/id
  ```

  This should report fake `uid=0(root)`. For a pushed runtime component,
  stage it under `/data/local/tmp`, then copy it with one quoted `run-as`
  command so the whole command stays inside the app uid:

  ```sh
  adb push FILE /data/local/tmp/FILE
  adb shell "run-as org.dn.shell sh -c 'cp -f /data/local/tmp/FILE /data/user/0/org.dn.shell/files/prefixes/dn-stock/DEST && chmod 755 /data/user/0/org.dn.shell/files/prefixes/dn-stock/DEST'"
  ```

  Do not uninstall the app or remove prefixes without explicit approval.
- No `sudo`/root anywhere. Persisted files (code, docs, commits) are in
  **English**.
- **Docs state only the current truth.** No history narrative; history lives
  in git.
- After moving/renaming/deleting a doc or a script, run
  `tools/check-repo.py` (links, anchors, reachability), plus
  `check-modules.py`, `check-interface.py` and `check-paths.py`.

## Priority: ship the idea fast, don't study every failure

The owner's time is the scarce resource. Build the idea the **fastest testable
way**; dig into a failure only when the shortest path is actually blocked.

- Choose the approach that directly exercises the idea.
- **Verify the change builds** before trusting or shipping it.

## Rule

**Run one command at a time.** No chaining, no parallel jobs, no `-P` fan-out.
Wait for the command to finish and read its output before starting the next.
