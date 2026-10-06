# The tracer: `dn-trace`

<!-- template: templates/docs.template.md -->

What the syscall tracer is for now that the fused loader exists, how it was measured,
and what is still open. Living doc, not per-release — last major update
2026-09-27 on `dev-0.2.0`, tested on `fe2`. Code: `../../tracer/` (its
`README.md` keeps the per-file prune list);
earlier background: [`direct-usage.md`](direct-usage.md),
[`bind-only.md`](bind-only.md), [`syscall-boundary.md`](../../reference/syscall-boundary.md).

## Contents

- [Where it sits](#where-it-sits)
- [What changed in 0.2.0](#what-changed-in-020)
- [Tests](#tests)
- [Measurements](#measurements)
- [Open](#open)

## Related docs

- [`direct-usage.md`](direct-usage.md), [`bind-only.md`](bind-only.md),
  [`syscall-boundary.md`](../../reference/syscall-boundary.md) — the earlier background
  (investigation, bind-only audit, the boundary map).
- [`android-platform.md`](../../reference/android-platform.md) — the Android enforcement
  gates this tracer's SIGSYS emulation works around.
- [`path-shim.md`](../shim/path-shim.md) — the shim this tracer is the fallback
  for.

## Where it sits

A Debian program in the prefix normally never meets the tracer:

| program | how it runs | tracer? |
|---|---|---|
| dynamic glibc (every translated `.deb`) | its `PT_INTERP` is the prefix's own fused glibc loader, which reads the path shim from `ld.so.preload` and finds libraries via `ld.so.cache` | no |
| script | its `#!` points into the prefix (or `dn-shell`/`dn-perl`) | no |
| **static binary** | no loader, the shim cannot see it | **yes** |
| **makes its own syscalls** (inline `svc`, `syscall()`) | the shim cannot see them (`scan-direct-syscalls.py`) | **yes** |
| **glibc NSS lookups** (`getpwnam`, `getaddrinfo`, ...) | libc-internal, not interposable | **yes** |

`core/native/dn-run.c` routes the last three to `usr/lib/deb-native/dn-trace`
(no fallback to Termux's `proot` since 0.2.3). A static binary also *needs* the
tracer to survive: Android's app seccomp filter kills calls such as
`set_robust_list` with SIGSYS (untraced, Debian's static `busybox find` dies),
and the tracer's SIGSYS emulation (`core/tracer/tracee/seccomp.c`) answers them.

## What changed in 0.2.0

### 1. Built at setup

`core/install/setup-runtime.sh` builds `core/tracer/` (`make CC=clang`, from clean) when
`make` and `libtalloc` are installed and copies `core/tracer/dn-trace` into the
prefix. Without them it prints

    W: tracer not built (needs: pkg install make libtalloc); static programs will run untranslated

and `dn-run` warns and runs those programs untranslated. Both
packages are optional requirements in the README.

### 2. `dn-trace` front end

`core/tracer/cli/dn-trace.c` replaces PRoot's command line (`cli/cli.c`,
`cli/proot.c`: option tables, help, qemu, `-r`/`-w`/`-0`, the pruned
extensions' options):

    dn-trace [-v LEVEL] [-b HOST[:GUEST]]... [--] PROGRAM [ARG...]

- guest root is always the host `/`, cwd is the current directory;
- a `-b` whose host path does not exist is skipped (PRoot warned);
- the arguments are a subset of `proot`'s.

### 3. The kernel execs the program; PRoot's loader is gone

PRoot execs its own loader in place of every program and has it map the
program and its ELF interpreter, so that a `PT_INTERP` naming a guest path
(`/lib/ld-linux-aarch64.so.1` inside a rootfs) can be found. In a deb-native
prefix every interpreter already names a host path — the prefix's own
glibc loader, Termux's glibc loader, Bionic's `linker64` — and static
programs have none. So `execve`
now translates only the program path (and a script's `#!` interpreter,
`execve/shebang.c`) and lets the kernel load it. `/proc/self/exe` is still
emulated from the guest path committed after a successful `execve`.

Removed with the loader (−2,774 lines, binary 191 → 153 KB):

- `loader/` and the load script (`execve/exit.c`);
- `syscall/heap.c` — `brk` emulation, only needed because the kernel's heap
  followed the loader, not the program;
- `execve/ldso.c`, `execve/auxv.c`, the qemu runner;
- the arm64 `PTRACE_POKEDATA` workaround: its stub ran inside the loader. It
  was used only when `process_vm_writev` also fails; `fe2` never needs it.

**Limit:** an untranslated Debian ELF (interpreter still
`/lib/ld-linux-aarch64.so.1`) now fails with ENOENT under the tracer.
`dn-translate-deb.sh` rewrites every ELF at install, so this only affects a
binary that bypassed it.

### 4. Bug fixed: `/proc` under the bind-only fast path

The fast path (`path/path.c`) let the kernel resolve `/proc/self/exe`; under
the loader that was the loader itself, so Debian's `busybox-static`, which
re-executes itself for every applet, died with SIGBUS on `sh -c 'cat ...'`.
Paths under `/proc` now go through `canonicalize()`, which emulates those
links. (With the loader gone the kernel's answer would now be the host path
of the program; the emulation keeps it the guest path.)

### 5. No termux-exec inside the tracer

`dn-run` used to keep `LD_PRELOAD` (Termux's `libtermux-exec`) on the static
and `--trace` routes. A Bionic program under the tracer then had its
`execve("/usr/...")` rewritten to `$PREFIX/...` *before* the tracer saw it
(found while testing: the first exec failed until `LD_PRELOAD` was unset).
Every tracer route now unsets `LD_PRELOAD` and `DN_BIONIC_PRELOAD`; tested
through `dn-run` with a Bionic `sh` child exec'ing `/usr/bin/busybox`.

### 6. Seccomp acceleration: kept on (a retracted change)

PRoot's own seccomp filter (stop only on syscalls it rewrites) first looked
like it cost ~360 ms per start, and was briefly made opt-in. Interleaved
re-runs showed no difference (~81 ms either way), so upstream's default was
restored. The lesson is in "How to measure".

## Tests

Run on `fe2` with Debian's `busybox-static_1.37.0-6+b9_arm64` unpacked in
`$TMPDIR/tracer-test` (`bb/usr/bin/busybox`), a fake `root/etc/probe`, and a
script `s.sh` with `#!/usr/bin/busybox sh`:

    env -u LD_PRELOAD dn-trace -b $T/root/etc:/etc -b $T/bb/usr:/usr -b $T/missing:/opt \
      /usr/bin/busybox sh -c "cat /etc/probe; ls /etc/../etc; readlink /proc/self/exe; \
        busybox readlink /proc/self/exe; $T/s.sh; $PREFIX/bin/cat /etc/probe; \
        $PREFIX/glibc/bin/bash -c 'echo \$BASH_VERSION; cat /etc/probe'; exit 5"

All pass: bound `/etc` read, `..` across a bind, a missing bind skipped, a
bind onto `/usr` (absent on Android), applet re-exec,
`/proc/self/exe` = `/usr/bin/busybox`, a guest-only `#!`, a Termux Bionic
and a Termux glibc child, exit code 5 passed through; a missing program
gives one error and exit 1.

Not yet run: the whole route `dn-run` → `dn-trace` from an installed prefix
(`fe2` had no `~/.dn` at the time).

## Measurements

Interleaved runs, minimum of each (`fe2`):

| | before | after |
|---|---|---|
| traced start, `busybox true` | ~81 ms | ~75 ms |
| 21 execs (`sh` loop of `busybox true`) | 421 ms (loader) | 388 ms (kernel exec) |
| `find $PREFIX/lib $PREFIX/share/doc` | ~0.5 s | ~0.5 s |
| untraced start, for reference | ~8 ms | |

### How to measure

Single timings on `fe2` swing from ~50 ms to ~900 ms for the same command
(CPU frequency, background load). Compare two builds or modes by
**interleaving** them in one loop and keeping the **minimum** of each;
never compare two separate batches. For an A/B against the last commit, build
it in a worktree (`git worktree add $TMPDIR/dn-old HEAD`) and run both
binaries in the same loop.

## Open

- [x] **`proot` fallback dropped** in `dn-run.c` (0.2.3): the loader, the
  shim and `dn-trace` cover the prefix; without `dn-trace`, `dn-run` warns
  and runs untranslated.
- **Small leftovers:** `/proc/self/auxv` handling in `syscall/exit.c` (now
  inert), the loader fields in `execve/execve.h`, `ptrace/` bookkeeping for
  loader syscalls. `path/glue.c` (PRoot's placeholder dirs for bind targets
  that do not exist on the host, such as `/usr`) likely stays; check before
  removing it.
- **Real workloads:** a static Go daemon (Tailscale, `tailscale.md`).
