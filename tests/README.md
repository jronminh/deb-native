# tests/

On-device smoke tests. Run in Termux; not run in CI (no Android device
there).

- `shim-libc/` — asserts every libc entry point `native/path-redirect.c`
  intercepts actually rewrites a path under `/etc` or `/usr` into a fake
  prefix root. See [`../docs/spec/shim-coverage.md`](../docs/spec/shim-coverage.md).
- `tracer-nss/` — proves the tracer route resolves glibc's statically-bound
  NSS reads (`getpwnam`, ...) against the prefix, the one case the shim
  cannot reach. See [`../docs/spec/syscall-boundary.md`](../docs/spec/syscall-boundary.md).
