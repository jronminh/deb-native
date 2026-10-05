# native/

<!-- template: templates/readme.template.md -->

The small set of C programs translated Debian programs actually run
through. Built by `bootstrap/setup-runtime.sh` into each prefix;
design background in [`../../docs/spec/shim/path-shim.md`](../../docs/spec/shim/path-shim.md)
and [`../../docs/spec/design.md`](../../docs/spec/design.md).

- `dn-shim.c` — the shim: a library that rewrites
  `/usr /etc /var /opt /root` into the prefix for glibc dynamic binaries,
  loaded via the prefix's `etc/ld.so.preload`.
  A Bionic build of the same idea for maintainer scripts was tried and
  abandoned — removed from the tree; see
  [`../../docs/spec/shim/path-shim.md`](../../docs/spec/shim/path-shim.md), "Dead end,
  fully explored" for the record.
- `dn-sh.c` / `dn-perl.c` / `dn-child.h` — the maintainer-script
  interpreters: two tiny glibc ELFs (shared setup in `dn-child.h`) the kernel
  runs from a shebang (not a shell script — the kernel follows only one `#!`
  level). Built by `install-runtime.sh` with the prefix's own gcc, repointed
  at the fused loader.
- `dn-run.c` — the runtime launch dispatcher: classifies a target's ELF
  at launch and picks the shim, plain exec, or the tracer.
