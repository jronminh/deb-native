# dn-ng: the next-generation APK runtime

Status: **proposal** (2026-10-19).  Not in 0.9.0.  `dn-ng` is the successor to
the current app/runtime pair for a **high `targetSdk`** and a single
**APK-only** distribution: no Termux at runtime, no separate runtime binary, no
external prefix, no toolchain on the device.

The existing law, contract and per-call behaviour stay the core
([`../architecture.md`](../architecture.md), [`contract.md`](contract.md)); what
changes is packaging, the exec path, and the host boundary.  The 0.9.0 runtime
keeps working for Termux/ADB and for `targetSdk 28`; dn-ng is the path for
everything newer.

## Contents

- [Why a new generation](#why-a-new-generation)
- [Goals and non-goals](#goals-and-non-goals)
- [Constraints on a high targetSdk](#constraints-on-a-high-targetsdk)
- [Packaging](#packaging)
- [The exec path](#the-exec-path)
- [The shim](#the-shim)
- [The prefix image](#the-prefix-image)
- [App and service](#app-and-service)
- [Boot flow](#boot-flow)
- [Security model](#security-model)
- [Updates](#updates)
- [Compatibility and hosts](#compatibility-and-hosts)
- [Milestones](#milestones)
- [Acceptance tests](#acceptance-tests)
- [Risks](#risks)
- [Open questions](#open-questions)
- [References](#references)

## Why a new generation

The current app works because the runtime and the prefix live in the app's data
directory and the app is `targetSdk 28`: in the `untrusted_app_27` domain,
`execve` of the prefix's own files and `execmod` for the shim's libc patches are
allowed.  Both are the wrong side of the newer app sandbox:

- from API 29 the app domain may not `execve` files in its own data directory;
- `targetSdk 28` is a temporary escape hatch, not a product for Play or a
  long-lived app;
- newer devices may use 16 KB pages, and dynamic-code-loading rules constrain
  what a writable tree may execute;
- 0.9.0's exec gate already rewrites `execve(PROG)` to "run the loader on PROG",
  so the missing piece is *where the loader comes from* and *how it gets there*
  without an app-data `execve`.

The answer is not "more fake kernel".  It is a different packaging: the runtime
and a loader are **APK native libraries**; the prefix image is an asset; the
loader performs a userland exec for the prefix's own loader and programs.

## Goals and non-goals

**Goals**

- One signed APK per ABI and Debian suite, installable on a stock phone.
- `targetSdk 36` (Android 16) as the first target; no `targetSdk 28` fallback
  in dn-ng.
- The prefix is still an unmodified Debian arm64 tree, with the 0.9.0 law and
  contract; the app is the host, not a second path layer.
- The runtime is updated by APK update; the prefix's state is separate and
  survives.
- No root, no SELinux bypass, no Termux at runtime.
- Data devices are allowed through the device table; authority is not.
- 16 KB-page builds are first-class; arm64 only in v1.

**Non-goals**

- A security sandbox.  A prefix is a compatibility layer; if isolation is the
  requirement, use a VM (AVF/Firecracker) or seL4/CHERI.
- Running arbitrary native code from writable app data.
- A general kernel-module, DRM/KMS, userns or module-loading path.
- Supporting `targetSdk 28` in the same package; legacy remains the old app.
- Replace Termux or the ADB host; the core still serves them.

## Constraints on a high targetSdk

The following are design constraints, not all separately verified here; the
exact API-level behaviour is a test matrix (see "Acceptance tests") and belongs
to the host, not the runtime law.

| constraint | consequence for dn-ng |
| --- | --- |
| `untrusted_app` at a high targetSdk | no root, no SELinux bypass; only app permissions |
| `execve` from app data refused | the runtime/loader must come from the APK; guest exec is userland |
| W^X / `execmod` tightening | do not rely on patching file-backed libc text; plan anonymous RX copies or a different shim strategy |
| dynamic code loading rules | code loaded at runtime must be read-only and from a trusted location; prefer APK libs |
| 16 KB page size | all native libs built/aligned for 16 KB; test prefix ELF mapping |
| APK size | a prefix is ~100 MB compressed; use assets/assets packs, one suite per APK |
| foreground service types | declare a type (likely `specialUse` with a property) and the notification permission |
| release signing | one key, kept offline; APK updates replace runtime libs |
| ABI | `arm64-v8a` only in v1; a 32-bit prefix is out of scope |

`targetSdk 36` is a policy choice; 35/36 are expected to be equivalent for this
design.  The number is not a runtime constant.

## Packaging

```
AndroidManifest.xml                 targetSdk 36, minSdk 26, arm64-v8a
lib/arm64-v8a/
  libdnloader.so                    userland-exec trampoline; dlopen'ed by the app
  libdntiny.so                      dn-tiny as a position-independent runtime
  libdnlane-<glibc>.so              shim variant, built for the prefix's glibc
  libdnexec.so                      optional: the exec gate's userland exec
  libdndevice.so                    host device backends (later)
assets/
  bootstrap.tar.gz                  the prefix image (or split assets)
  config.schema
res/...
```

- `libdnloader.so` is the only entry point the app calls: it maps the prefix's
  own `ld-linux-aarch64.so.1`, builds `argv`/`envp`/`auxv`, and jumps.  It is a
  small, audited loader; it is not a second path law.
- `libdntiny.so` is the tracer, built **without libc** (the current
  `build/dn-tiny` already has no imports) and exposed as a callable entry.  The
  app forks a tracee and the tracer runs in the parent (or in a child created
  with `clone`); no app-data `execve` is needed for the runtime.
- The shim is a **glibc DSO**; it cannot be one universal file.  Ship variants
  by glibc/suite ABI, or build it into the prefix image and load it read-only
  (see "The shim").  The app does not link it.
- The prefix image is an asset.  It is unpacked once into `files/`; an app
  update replaces the APK libs and leaves `files/` alone.
- `android:extractNativeLibs` is a test axis: executing a lib from the APK's
  native directory may be allowed where `execve` of app data is not.  The
  preferred design is **no execve of libs at all**: `dlopen` the loader and use
  userland exec.  If the platform permits executing the APK lib directory,
  that is a fallback for the tracer, not a requirement.

## The exec path

The central change.  0.9.0's gate rewrites:

```
execve(PROG)  ->  execve(prefix/ld.so, PROG)
```

dn-ng cannot rely on either `execve` being allowed at a high targetSdk.  The
design is:

```
execve(PROG)
  -> runtime sees the gate
  -> libdnloader maps prefix/ld.so + PROG (and a static PROG directly);
     the prefix's ld.so maps libc and the rest
  -> builds the stack and auxv exactly as the kernel would
  -> jumps into the prefix's ld.so entry with the program as its argument
  -> the process is now the Debian program, under the same tracer
```

- For a dynamic program, the **prefix's own** `ld-linux` and `libc.so.6` are
  mapped; never mix the APK's bionic loader with the tree's glibc.
- For a static program, the loader maps the ELF and jumps to its entry.
- `/proc/self/exe` and the exec gate already answer the guest program name
  (`exe_now`); the loader must preserve that view.
- The tracer's own start is the same trick: the app forks a child that maps
  `libdntiny.so` and jumps to its entry; that child is the tracer.  It forks
  the tracee, which userland-execs the guest.  No app-data `execve`.
- Every guest `execve` therefore goes through the loader; the kernel sees an
  `mmap` of the tree's files, not an `execve` of them.

This is **believed**, not checked on a high targetSdk; the first milestone is
exactly this path with a static "hello".  The TODO's checks (execve of app
data, `dlopen` of a data file, `mmap(PROT_EXEC)`, `execmem`, `fexecve` of a
memfd) are the evidence to gather before the design is called settled
(`TODO.md`, "A prefix in any app").

A userland exec is not just "jump to the entry".  The kernel normally does work
the loader must repeat: close `FD_CLOEXEC` descriptors, reset signal
dispositions and the alternate stack, clear the robust futex list, set
`comm`/`argv`/`auxv`, handle `#!` and `binfmt_misc`, and produce the
`PTRACE_EVENT_EXEC` the tracer expects.  Missing any of these is a compatibility
bug and sometimes a security one; the loader's conformance tests include
scripts, `CLOEXEC`, signal handlers, and exec under the tracer.

Fallbacks, in order, if the primary path fails:

1. execute the APK's native library directory, if the platform allows it;
2. copy code into anonymous memory and `mprotect(RX)` (the JIT-style path);
3. stop and use AVF/Terminal, rather than weaken the app sandbox.

## The shim

The shim is not optional for a glibc tree: it is the fast path and the in-process
half of the law.  It is also a DSO built against the tree's glibc and it needs
`execmod` or an anonymous-text equivalent to patch libc's `svc` sites.

Two placements are on the table:

- **APK lib**: `libdnlane-<glibc>.so` in the APK, opened read-only by the
  prefix's ld.so.  The guest path to the APK lib directory is not under the
  tree; the runtime must expose it through a narrow, read-only row/bind, or the
  loader can map it directly.  This keeps code out of the writable prefix.
- **Prefix image**: the shim in the tree, mode `0555`, loaded by the prefix's
  ld.so.  This is the 0.9.0 shape and the simplest path, but dynamic-code and
  W^X rules may refuse it at a high targetSdk.

The decision is by test, not by preference.  In both cases:

- one shim variant per glibc/suite; the app selects it from `boot/version`;
- never mix a shim built for another glibc;
- if `execmod` is refused, patch into anonymous RX pages and remap; if that is
  refused too, the shim is not viable on that host and the runtime must say so
  rather than run slower and insecure;
- the shim is never the enforcement point (see [`syscall-security.md`](../reference/syscall-security.md)).

## The prefix image

- Built by `scripts/build-prefix.sh` outside the app, then packed as
  `assets/bootstrap.tar.gz` (one suite per APK).  The image is data; it never
  contains the runtime or the shim unless the shim placement chooses that.
- The tree is unpacked into `files/` once; a fresh install or a data wipe
  unpacks again.  `files/` is writable, as Debian expects; the runtime is not.
- A 16 KB page host is a build variant: the prefix's ELF files may need
  `-Wl,-z,max-page-size=16384` or a userland loader that maps them correctly.
  The loader is the first place to test; do not assume a stock trixie tree
  executes unchanged on a 16 KB kernel.
- Size: one suite APK is expected to be ~100-200 MB; if Play is a target, split
  by suite/asset pack, not by downloading arbitrary code.

## App and service

- A foreground `RuntimeService` owns the session; a `SurfaceView`/terminal UI
  binds to it, as today.
- `targetSdk 36` declarations: foreground service type (prefer `specialUse`
  with a documented property), `POST_NOTIFICATIONS`, `INTERNET`; no storage
  permission (`SAF` for user files).
- The app is the **host** ([`host.md`](host.md)):
  - boot inputs (`boot/binds`, `boot/env`) before boot;
  - channels for Android bridge, display/input, clipboard, audio;
  - devices through the device table ([`devices.md`](devices.md)), data only.
- The app must not intercept paths or syscalls; that remains the runtime's law.
- Debug builds may keep `run-as`; release builds must not rely on it.

## Boot flow

1. App starts; `RuntimeService` starts foreground; the prefix tree is prepared
   from assets if missing.
2. `libdnloader.so` is loaded; the tracer (`libdntiny`) is started in the
   parent/child model.
3. The tracee userland-execs `ROOT/boot/boot.sh` (or `bash`), with the tree as
   `/` and the host inputs already written.
4. The session is attached to the app's terminal/surface; the Android bridge
   and device providers connect if the prefix asked for them.
5. On app update, the APK libs are replaced; `files/` persists.  A runtime
   restart is a new boot of the same tree.

## Security model

- The app is `untrusted_app`; dn-ng does not change that.  Packaging the runtime
  in the APK is not a privilege and must not be described as one.
- The runtime is a compatibility layer; its fake root, `/proc` answers and
  synthetic devices are **data**, not authority.  Authority remains the app's
  permissions.
- The tracer, not the shim, is the enforcement point for anything security
  relevant; the shim runs in the untrusted program's address space.
- The device table's provider is trusted, the guest is not; every ioctl is a
  parser; no host paths or arbitrary fds cross.
- `ptrace`, `userns`, `io_uring`, `bpf`, `perf`, raw sockets and module loading
  remain outside the promise; a high targetSdk makes that easier, not harder.
- If a workload needs isolation, the answer is AVF/Firecracker or seL4/CHERI,
  not a bigger prefix.  The bypass classes are in
  [`syscall-security.md`](../reference/syscall-security.md).

## Updates

- APK update = runtime/shim/host update.  The prefix's `files/` and installed
  packages are not touched.
- The shim variant is selected from `boot/version`/glibc; a mismatch is refused
  at boot, not silently loaded.
- No in-prefix `dn-update` in dn-ng's core: a writable overlay of the runtime is
  exactly the dynamic-code path the high targetSdk is closing.  A future
  allowlisted update may return, but only with a signature and a read-only
  staging area.
- The prefix can still `apt install` into the tree; that is data.

## Compatibility and hosts

- **dn-ng** is the APK host at `targetSdk 36`; it does not support
  `targetSdk 28`.
- **Legacy app** remains for `targetSdk 28` and for devices where the exec path
  is allowed; it is not the product path.
- **Termux** and **ADB** hosts keep 0.9.0's behaviour; the runtime core is the
  same, the host differs.
- **AVF/Terminal** is the alternative when a real VM is wanted; dn-ng does not
  compete with it for isolation.

## Milestones

| | scope | proves |
| --- | --- | --- |
| **NG0** | APK at `targetSdk 36` with `libdnloader`; userland-exec a static hello | the loader path; no app-data execve |
| **NG1** | `libdntiny` as a callable runtime; fork/clone tracer; seccomp; a static guest | runtime embedding, no exec of the runtime |
| **NG2** | prefix asset; dynamic `bash`/`apt` through the loader; shim placement chosen | a real Debian tree |
| **NG3** | terminal/PTY + Android bridge; foreground service; release signing | a usable app |
| **NG4** | 16 KB page build and an Android 16 device; negative/denial tests | the high-Android target |
| **NG5** | device table (fbdev/evdev) and app backends | optional I/O |
| **NG6** | distribution (F-Droid/GitHub/Play if allowed) | packaging |

NG0-NG2 do not need the device table or a compositor; they are the critical
path.

## Acceptance tests

- Install a release-signed APK at `targetSdk 36` on Android 16; app starts with
  no `run-as`.
- In the guest: `id`, `ls`, `apt-get update`, `dpkg`, `gcc`, `git`, a threaded
  program, `vfork` + `chdir`.
- Loader conformance: `#!/bin/sh` scripts, `CLOEXEC` fds, signal dispositions
  and altstack reset, exec under the tracer, and a two-level interpreter chain.
- `execve` of a file under `files/` is **not** used by the runtime; audit the
  trace and the loader.  `/proc/self/exe` reports the guest program.
- `grep Seccomp /proc/self/status` is 2; no unexpected SELinux denial.
- The prefix boots with the tracer in the APK and the shim chosen by
  `boot/version`; a wrong shim is refused.
- 16 KB: the loader and a small Debian binary run on a 16 KB page host; if not,
  the build variant is documented and refused rather than half-working.
- No kernel `execve` of a path under `files/` is observed.  The userland loader
  maps the guest ELF/ld.so with executable protection (that is the point); the
  audit is about the `execve`, not about the mappings.

## Risks

- The exec path may be refused even for the APK lib directory; the anonymous-RX
  fallback may be the only option, and it may be closed later.
- A userland exec does not emit `PTRACE_EVENT_EXEC`; the tracer must synthesize
  the post-exec view, and a shebang/interpreter chain is the hard case.
- `execmod` refusal may make the current shim unusable; a new text-patching
  strategy is a project in itself.
- 16 KB prefix compatibility may require a rebuilt tree; a stock trixie image
  may not be enough.
- APK size and update size; a 100-200 MB APK is normal for this class, but
  distribution is constrained.
- `targetSdk` policy can move again; the design must not depend on a specific
  number beyond "high".
- The app and runtime can drift if the shim is not tied to the tree; the boot
  check is mandatory.

## Open questions

1. **Exec source**: APK lib `execve`, `dlopen` + userland exec, or anonymous RX?
   NG0 decides.
2. **Tracer process model**: fork from the app process, `clone`, or a separate
   process started from a lib?  Which one keeps `ptrace` and seccomp clean,
   and who synthesizes `PTRACE_EVENT_EXEC` after a userland exec?
3. **Shim placement**: APK lib vs read-only tree; does either survive the
   dynamic-code-loading and `execmod` rules on Android 16?
4. **16 KB**: does the userland loader make a 4 KB-aligned Debian ELF run, or
   does the prefix need a rebuild?  Which toolchain?
5. **Foreground service type**: `specialUse` is the likely answer; what is the
   exact declaration and review story?
6. **Distribution**: sideload/F-Droid only, or Play with an asset pack?  A
   200 MB base APK is not a Play-friendly shape.
7. **Shim ABI**: how many glibc variants ship, and how does the app choose
   without a writable runtime?
8. **AVF boundary**: at what point is the honest answer "use the Terminal/VM"
   instead of a bigger APK?

## References

[`../architecture.md`](../architecture.md) (the law), [`contract.md`](contract.md),
[`host.md`](host.md), [`hosts.md`](hosts.md), [`devices.md`](devices.md),
[`../reference/syscall-security.md`](../reference/syscall-security.md),
[`../building.md`](../building.md), TODO, "A prefix in any app".
