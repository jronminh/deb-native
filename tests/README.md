# tests/

<!-- template: templates/readme.template.md -->

On-device smoke tests. Run in Termux; not run in CI (no Android device
there).

- `shim-libc/` — asserts every libc entry point `native/path-redirect.c`
  intercepts actually rewrites a path under `/etc` or `/usr` into a fake
  prefix root. See [`../docs/spec/shim/shim-coverage.md`](../docs/spec/shim/shim-coverage.md).
- `tracer-nss/` — proves the tracer route resolves glibc's statically-bound
  NSS reads (`getpwnam`, ...) against the prefix, the one case the shim
  cannot reach. See [`../docs/reference/syscall-boundary.md`](../docs/reference/syscall-boundary.md).
- `glibc-swap/` — the 0.6.0+s.1 (`s` = swap in deploy) acceptance test for
  an **already-deployed** prefix (it never bootstraps or downloads). Run
  `run.sh PREFIX [fresh|live]`: `fresh` checks the whole swap (Debian's
  `libc6`/`libc-bin` installed and held, this project's own loader,
  every `PT_INTERP`, the shim, the cache, runs, NSS); `live` runs the same
  functional checks on an upgraded prefix, tolerant of a not-yet-migrated
  package set. See
  [`../docs/spec/dn-glibc-prefix.md`](../docs/spec/dn-glibc-prefix.md).
