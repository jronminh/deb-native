# tracer/ — fork-lite

The project's syscall-level path tracer: a reduced subset of **PRoot**
(<https://github.com/termux/proot>), kept **arm64-only** and trimmed to path
handling. It is first-class project code, not a `third_party/` vendoring; the
derived files keep proot's GPLv2-or-later headers and copyright.

Why it exists: rewriting a syscall's path arguments requires `ptrace` —
seccomp user-notification can inspect and inject fds but cannot modify
arguments — so proot's `ptrace` core is the hard part worth reusing. See
[`../docs/spec/direct-usage.md`](../docs/spec/direct-usage.md).

## Status

**AArch64-only.** The extension suite is gone (framework `extension.c` kept)
and the multi-arch machinery is removed: `arch.h` is AArch64-only with a hard
`#error` otherwise, the 32-bit ARM ABI and PRoot's loader are gone, and the
`sysnums-{arm,i386,x86_64,x32,sh4}.h` / `assembly-{arm,x86,x86_64}.h` files
are deleted. It builds on Termux (`make CC=clang`, needs `libtalloc`) and a
static binary reads through a bind. **The bind-only fast path has landed**
(see below), and so have the `dn-trace` front end and kernel exec (see
"Prune plan").

## Bind-only fast path (deb-native-specific)

`translate_path` (`path/path.c`) no longer walks every component with
`lstat(2)`. It normalizes the guest path (collapse `.`/`//`, keep one trailing
`/`) and prefix-substitutes the leading bound component, letting the kernel
resolve the rest. This is correct only because deb-native's scope guarantees
rootfs `/` (no chroot), flat top-level binds, and a symlink-normalized guest
tree. **It is not a general proot optimization** — it assumes what stock proot
cannot: see [`../docs/spec/bind-only.md`](../docs/spec/bind-only.md).

Safe mechanics for the three traps:

- **absolute symlinks** — `scripts/install/normalize-symlinks.sh` rewrites absolute
  targets under bound dirs to relative; run by `install.sh` after install.
- **`..` across a bind** — detected in `normalize_guest_path()`, falls back to
  `canonicalize()`.
- **`/proc` links** — paths under `/proc` also fall back to `canonicalize()`,
  which emulates `/proc/<pid>/exe` and friends; under PRoot's loader (since
  removed) the kernel's `/proc/self/exe` was the loader (a static busybox re-executing
  itself for an applet died with SIGBUS).
- **output detranslation** — `detranslate_path` unchanged (getcwd, readlink,
  `/proc/self/cwd`).

`PROOT_NO_BIND_ONLY=1` forces the old canonicalize path (A/B and escape hatch).

Benchmark (`scripts/bench/bench-tracer.sh`, medians on `fe2`, binds `$PREFIX:/usr`):

| workload | og (stock proot) | fork-lite canonicalize | fork-lite bind-only |
|---|---|---|---|
| 20 000 path lookups, one process | 2.26s | 2.26s | **1.40s (~1.6x)** |
| `find -type f` over 67k files | 2.66s | 2.80s | 2.45s (~8%) |

The stat loop isolates translation cost (og ≈ fork-lite-canonicalize, so the
gain is the fast path, not the extension prune); the walk is I/O-bound.

## Origin

- upstream: `termux/proot` @ `d4d2a19081c3c07f75250e4ce2980b9fa2f5720f`
  (2026-09-24), itself a fork of `proot-me/PRoot`.
- license: GPL-2.0-or-later (see each file's header and `../LICENSE`).
- imported from a `--depth 1` clone on 2026-09-26.

## Build

```
cd tracer
make CC=clang        # produces ./dn-trace
```

`setup-runtime.sh` does this when `make` and `libtalloc` are installed
(`pkg install make libtalloc`) and copies `dn-trace` into the prefix as
`usr/lib/deb-native/dn-trace`.

## Prune plan (fork-lite)

Keep:

- `ptrace/`, `tracee/`
- `syscall/{enter,exit,seccomp,chain,sysnum}.c` and `sysnums-arm64.h`
- `path/`, `execve/`, `arch.h`, `compat.h`
- `extension/{extension.c,extension.h}` — the framework only (core call sites
  keep linking; with no extension initialized, its hooks are no-ops).

Drop (done):

- `extension/*/` — every concrete extension (`fake_id0`, `link2symlink`,
  `sysvipc`, `ashmem_memfd`, `kompat`, `hidden_files`, `mountinfo`,
  `port_switch`, `fix_symlink_size`).
- the other-arch `sysnums-*.h`.
- `cli/cli.c`, `cli/proot.c` — PRoot's command line, replaced by
  `cli/dn-trace.c`: `dn-trace [-v LEVEL] [-b HOST[:GUEST]]... [--] PROGRAM`,
  guest root = host `/`, cwd = the current one, a `-b` with a missing host
  path skipped. The same arguments work with Termux's `proot`, which
  `native/dn-run.c` falls back to.
- `loader/`, its load script (`execve/exit.c`), `execve/ldso.c`,
  `execve/auxv.c`, `syscall/heap.c` (brk emulation) and the qemu runner:
  the kernel now execs the translated program itself (`execve/enter.c`).
  PRoot's loader exists to map a program whose `PT_INTERP` is a guest path;
  in a deb-native prefix every interpreter is a host path (ld-dn, Termux's
  glibc loader, Bionic's linker64) and static programs have none. Only the
  program path and a script's `#!` interpreter are translated, so an
  untranslated Debian ELF (`PT_INTERP` = `/lib/ld-linux-aarch64.so.1`)
  fails with ENOENT under the tracer; `dn-translate-deb.sh` rewrites them all.
  `/proc/self/exe` is still emulated from the committed guest path. Lost
  with the loader: the arm64 `PTRACE_POKEDATA` workaround (its stub ran in
  the loader), used only when `process_vm_writev` also fails. Measured on
  fe2 (interleaved minimums, 12 runs): 21 execs 421 → 388 ms, `find` the
  same; binary 191 → 153 KB.

## Seccomp acceleration: kept on

PRoot installs a seccomp filter so only the syscalls it rewrites stop the
tracee (`PROOT_NO_SECCOMP=1` turns it off). A first measurement on the test
device suggested it cost ~360 ms per start; interleaved re-runs
(2026-09-27, Debian `busybox-static`, minimum of 30 each) showed no
difference: ~81 ms either way. Single timings on that device swing from
~50 ms to ~900 ms, so compare interleaved minimums, not one run. Upstream's
default stays.

Its SIGSYS emulation (`tracee/seccomp.c`) is separate: it is what lets a
static binary survive Android's app seccomp filter (untraced,
`busybox find` is killed with SIGSYS).
