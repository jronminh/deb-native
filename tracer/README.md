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
static binary reads through a bind. Remaining: the `cli/` → binder rewrite.

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
