# tracer/ — fork-lite

The project's syscall-level path tracer: a reduced subset of **PRoot**
(<https://github.com/termux/proot>), kept **arm64-only** and trimmed to path
handling. It is first-class project code, not a `third_party/` vendoring; the
derived files keep proot's GPLv2-or-later headers and copyright.

Why it exists: rewriting a syscall's path arguments requires `ptrace` —
seccomp user-notification can inspect and inject fds but cannot modify
arguments — so proot's `ptrace` core is the hard part worth reusing. See
[`../docs/direct-usage.md`](../docs/direct-usage.md).

## Status

**AArch64-only.** The extension suite is gone (framework `extension.c` kept)
and the multi-arch machinery is removed: `arch.h` is AArch64-only with a hard
`#error` otherwise, the 32-bit ARM ABI and `-m32` loader are gone, and the
`sysnums-{arm,i386,x86_64,x32,sh4}.h` / `assembly-{arm,x86,x86_64}.h` files
are deleted. It builds on Termux (`make CC=clang`, needs `libtalloc`) and a
static binary reads through a bind. **The bind-only fast path has landed**
(see below). Remaining: the `cli/` → binder rewrite (`dn-trace`).

## Bind-only fast path (deb-native-specific)

`translate_path` (`path/path.c`) no longer walks every component with
`lstat(2)`. It normalizes the guest path (collapse `.`/`//`, keep one trailing
`/`) and prefix-substitutes the leading bound component, letting the kernel
resolve the rest. This is correct only because deb-native's scope guarantees
rootfs `/` (no chroot), flat top-level binds, and a symlink-normalized guest
tree. **It is not a general proot optimization** — it assumes what stock proot
cannot: see [`../docs/bind-only.md`](../docs/bind-only.md).

Safe mechanics for the three traps:

- **absolute symlinks** — `scripts/normalize-symlinks.sh` rewrites absolute
  targets under bound dirs to relative; run by `install.sh` after install.
- **`..` across a bind** — detected in `normalize_guest_path()`, falls back to
  `canonicalize()`.
- **output detranslation** — `detranslate_path` unchanged (getcwd, readlink,
  `/proc/self/cwd`).

`PROOT_NO_BIND_ONLY=1` forces the old canonicalize path (A/B and escape hatch).

Benchmark (`scripts/bench-tracer.sh`, medians on `fe2`, binds `$PREFIX:/usr`):

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
make CC=clang        # produces ./proot
```

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
  `port_switch`, `fix_symlink_size`). Their init calls live in `cli/proot.c`
  and `cli/cli.c` and are now no-ops.
- `loader/` m32 and the non-arm64 loaders (`HAS_LOADER_32BIT` removed from
  `arch.h`, `assembly-{arm,x86,x86_64}.h` deleted), and the other-arch
  `sysnums-*.h`.

Write ourselves:

- a small `main`/binder replacing `cli/`, which binds `$INSTDIR` over
  `/usr /etc /var /opt`, handles guest paths that do not exist (proot errors
  on them), and sets `DN_INSTDIR`/`PATH` — producing a `dn-trace` binary.

Then `native/dn-run.c`'s direct-usage route points at `dn-trace`, keeping
`proot` as the fallback.
