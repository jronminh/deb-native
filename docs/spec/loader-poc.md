# Loader POC: `dn-exec` and the FHS bootstrap

Status: **POC in progress** (2026-10-19), branch `dev/loader`.  L0/L1/L2 pass;
L3 (shim/ldhook) is next.  Not in 0.9.0 and not merged.  The target is the one seam left after the runtime is in place: at
boot, something must be loaded by the kernel before the prefix's own loader and
law can run.  This plan removes the prefix paths from that seam.

The design context is [`dn-ng.md`](dn-ng.md); the current loader behaviour is
[`../architecture.md`](../architecture.md) and
[`../reference/syscall-patching.md`](../reference/syscall-patching.md).

## Why a POC

The runtime already has a fast loader path (`dn-ldhook.so`) and a shim; the
remaining loader problems are structural, not a missing row:

- `execve` of a glibc program is currently rewritten to
  `execve(host loader, --preload host shim, --audit host ldhook, program)`.
  The program is a guest path, but the bootstrap objects are Android absolute
  paths; the kernel execs the loader from the app's data directory.
- `execve("/proc/self/fd/L")` works under the runtime, and
  `--preload/--audit /proc/self/fd/N` is accepted, but the test showed the
  identity gap: `/proc/self/exe` becomes `/proc/self/fd/10` because the exec
  gate does not know this is a loader.
- A shell test cannot fix the gate, the path identity, or the ldhook root.  A
  rebuilt runtime is required.
- The real target is not "fd bootstrap" for its own sake.  It is **userland
  exec** (`dn-exec`): the kernel execs a tiny trusted artifact once, and that
  artifact maps the prefix's own loader and program.  Then the prefix sees
  only its FHS `/`.

## Design

```
guest execve(PROG)
  -> exec gate rewrites to execve(DN_EXEC, PROG, argv, env)
  -> kernel execs DN_EXEC (small, static, no libc; APK lib or fd path)
  -> dn-exec maps prefix/ld.so + PROG (or a static PROG)
  -> builds stack/auxv; jumps to the prefix loader's entry
  -> the process is the Debian program, under the same tracer
```

Two properties matter:

1. **The kernel never execs a prefix file.**  It execs `dn-exec`; the prefix's
   loader and program are mapped by `dn-exec`.  The loader's own opens are guest
   paths and go through the law; the bootstrap DSOs can be guest paths or fds.
2. **`vfork`/`posix_spawn` stays safe.**  The `execve(DN_EXEC)` is a real kernel
   exec, so the vfork child gets a private mm before `dn-exec` runs.  The
   userland exec happens after that, not in the shared vfork mm.  This is the
   main reason to exec `dn-exec` rather than userland-exec in place.

`dn-exec` is the only new privileged-by-position artifact: it is trusted, tiny,
freestanding, and has no authority.  It validates the ELF it maps and never
reads a path from the guest without the law.

## POC result so far

Built `build/dn-exec` as a freestanding static PIE (`clang -static-pie
-nostdlib ... -Wl,-e,_start`), and `tests/dn-exec/hello` as a static ET_EXEC.

- A static hello runs through `dn-exec`.
- Under `dn-tiny` (`build/dn-tiny -t P -- /usr/local/bin/dn-exec ...`),
  `/usr/bin/true` and `find /usr -maxdepth 1` run; the loader's libraries are
  mapped through the law.
- One stop cost: `dn-exec /usr/bin/true` was `enter=6 paths=5`; the current
  kernel-exec path was `enter=3 paths=1` (single run, not a benchmark).
- L2 is in: `readlink /proc/self/exe` says `/usr/bin/readlink`,
  `/proc/self/comm` says `readlink`, and `/proc/self/cmdline` starts with the
  program; the tracer recognises `dn-exec` and sets `exe_next` from its argv.
  A subsequent `exec` (`env`, `bash -c exec`) still goes through the gate.
- The POC uses `MAP_FIXED` (the 4.14 kernel has no `MAP_FIXED_NOREPLACE`) and
  maps images from anonymous memory, so it needs `execmem` for RX, not
  `execmod`; this is the opposite trade of the current loader hook.

Build/run:

```sh
scripts/build-dn-exec.sh
tests/dn-exec/run.sh ~/work/p-adb
```

## What `dn-exec` must do

- Parse ELF64/AArch64: `PT_LOAD`, `PT_INTERP`, `PT_GNU_STACK`,
  `PT_GNU_RELRO`, `p_align`; reject anything outside the tree.
- For a dynamic program, map the prefix's `ld.so` and the program, build the
  kernel-style stack and auxv, and jump to the loader.  The program and the
  loader are addressed by guest path or fd; no Android absolute path in
  `argv`/`env`.
- For a static program, map it and jump to its entry; no loader.
- Handle `#!`/binfmt by rewriting `argv` and mapping the interpreter, as the
  kernel would.
- Set `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_BASE`, `AT_ENTRY`, `AT_EXECFN`,
  `AT_RANDOM`, `AT_PAGESZ`, `AT_PLATFORM`, `AT_HWCAP`, `AT_HWCAP2`,
  `AT_SYSINFO_EHDR`, `AT_UID/EUID/GID/EGID`, `AT_SECURE=0`.
- Reproduce the exec side effects the kernel would: close `FD_CLOEXEC`, reset
  signal dispositions and altstack, clear the robust futex list, set `comm`,
  and hand the tracer the program identity (`exe_now`) even though there is no
  `PTRACE_EVENT_EXEC`.
- 16 KB page size: map with the host's page size and set `AT_PAGESZ` from it;
  test a 4 KB-aligned Debian tree before deciding whether a rebuilt prefix is
  needed.

## What the runtime must do

- **Exec gate**: recognise `execve` of a program and rewrite it to
  `execve(DN_EXEC, ...)` preserving guest `argv`, `env`, and the program path.
  A feature switch (`DN_EXEC=dn-exec|kernel`) is needed for A/B.
- **Identity**: `dn-exec` tells the tracer which guest program it maps; the
  tracer answers `/proc/self/exe`, `/proc/self/cmdline`, `/proc/self/maps`,
  `AT_EXECFN`, and `exe_now` from that, not from `dn-exec`'s own path.
- **ldhook/shim**: load them as guest paths or fds.  `dn-ldhook.so` must find
  the prefix root even when its `dli_fname` is `/proc/self/fd/N`; pass the root
  through a narrow env/fd hint, or derive it from the mapped object's host
  path.  The shim has the same question.
- **No `PTRACE_EVENT_EXEC`**: the tracer must treat the jump as an exec
  boundary for its per-process state (program name, cwd, ids, exec gate).
- **Bootstrap fds**: `dn-exec`, the loader, shim and ldhook fds are opened by
  the host and inherited; `FD_CLOEXEC` cleared; numbers reserved.  The guest
  must not be able to swap them for its own files.

## Stages

| | scope | gate |
| --- | --- | --- |
| **L0** | done: `dn-exec` maps a static hello and jumps; no prefix loader | a static program runs |
| **L1** | done: dynamic hello: map the tree's `ld.so` and program; auxv/stack | `ld.so` loads libc and enters the program |
| **L2** | done: guest argv/env, `#!`, `/proc/self/exe`, `AT_EXECFN` under the tracer | `readlink /proc/self/exe` is the guest path |
| **L3** | next: shim + ldhook from guest paths/fds; root hint; no Android path in argv/env | `find`/`bash` boot; no host path in loader-visible config |
| **L4** | A/B against the current kernel exec: stops and wall on the three workloads | no worse than the baseline |
| **L5** | `posix_spawn`/`vfork`, 100 × `true`, `make`, Python subprocess | no parent corruption; spawn within the agreed bound |
| **L6** | 16 KB host and app `targetSdk` 36 (when available) | documented pass or refusal |

L0-L3 are the POC.  L4 decides whether the design is worth merging.  L5 is the
make-or-break for real workloads; L6 is the dn-ng gate.

## Measurements

Compare the same commands interleaved, on the S10e, in the ADB/Termux host:

- Lua build: wall, entry/exit stops, `ld.so`, `execve`, `unlinkat`, SIGSYS;
- 100 × `true`: wall and events (loader startup, fork/exec);
- `find /usr`, `git status`, `python os.walk`;
- boot time and `/proc/self/exe`/cmdline/maps checks;
- `vfork`/`posix_spawn` correctness, not just speed.

Success for the POC:

- no `execve` of a path under the prefix (only `DN_EXEC`);
- no Android absolute path in the loader's `argv/env/--preload/--audit`;
- `/proc/self/exe` and `AT_EXECFN` are the guest program;
- stops no worse than the current loader path; Lua wall within 5% of baseline;
- `spawn` within 10% of baseline and no parent corruption;
- a wrong `DN_EXEC`/shim is refused, not silently run.

## Non-goals

- Replacing the runtime's law or the shim.
- Device/GUI work.
- A general ELF loader for foreign architectures.
- Running from a writable prefix by kernel `execve`.

## Risks and decisions

- **APK lib exec**: `dn-exec` as a static executable named `libdnexec.so` may
  or may not be executable from the APK lib directory on a high targetSdk;
  test both `execve` of the lib dir and `/proc/self/fd/N`.
- **ldhook root**: if root cannot be passed without a path, the shim/ldhook
  may need the prefix root in a reserved env var; that must be stripped or
  answered on read.
- **`execmod`**: ldhook still patches loader text; if refused, map a private
  copy of the loader and patch before the jump.
- **16 KB**: a 4 KB-aligned prefix may need a rebuilt tree; decide after L6.
- **vfork**: expected safe because the kernel execs `dn-exec` first; prove it
  before relying on it.

## Branch work

- Branch: `dev/loader`.
- Small commits per stage; no merge to `main` until L4/L5 pass.
- First commit after this plan: `dn-exec` skeleton + a static hello test; no
  changes to the 0.9.0 path.
- The POC is not shipped in 0.9.x; a passing POC feeds [`dn-ng.md`](dn-ng.md).

## References

[`dn-ng.md`](dn-ng.md), [`../architecture.md`](../architecture.md),
[`../performance.md`](../performance.md),
[`../reference/syscall-patching.md`](../reference/syscall-patching.md),
[`../reference/prior-art.md`](../reference/prior-art.md) (userland exec, ulexecve,
Santa's ELFs), TODO, "A prefix in any app".
