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

**Imported base, not yet pruned.** It builds on Termux (`make CC=clang`, needs
`libtalloc`) and a static binary reads through a bind — validated on the
phone. The pruning below has not been applied yet.

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

Drop:

- `extension/*/` — every concrete extension (`fake_id0`, `link2symlink`,
  `sysvipc`, `ashmem_memfd`, `kompat`, `hidden_files`, `mountinfo`,
  `port_switch`, `fix_symlink_size`). Their init calls live in `cli/proot.c`
  and `cli/cli.c`; the CLI is being replaced, which removes them.
- `loader/` m32 and the non-arm64 loaders (`HAS_LOADER_32BIT` in `arch.h`,
  `assembly-arm.h`, the `loader-m32` rules).
- `sysnums-{arm,i386,x86_64,x32}.h`, other-arch register sets, QEMU hooks.

Write ourselves:

- a small `main`/binder replacing `cli/`, which binds `$INSTDIR` over
  `/usr /etc /var /opt`, handles guest paths that do not exist (proot errors
  on them), and sets `DN_INSTDIR`/`PATH` — producing a `dn-trace` binary.

Then `native/dn-run.c`'s direct-usage route points at `dn-trace`, keeping
`proot` as the fallback.
