# native/

<!-- template: templates/readme.template.md -->

The small set of C programs translated Debian programs actually run
through. Built by `scripts/install/setup-runtime.sh` into each prefix;
design background in [`../docs/spec/shim/path-shim.md`](../docs/spec/shim/path-shim.md)
and [`../docs/spec/design.md`](../docs/spec/design.md).

- `path-redirect.c` — the shim: a library that rewrites
  `/usr /etc /var /opt /root` into the prefix for glibc dynamic binaries,
  loaded via the prefix's `etc/ld.so.preload`.
  A Bionic build of the same idea for maintainer scripts was tried and
  abandoned — removed from the tree; see
  [`../docs/spec/shim/path-shim.md`](../docs/spec/shim/path-shim.md), "Dead end,
  fully explored" for the record.
- `dn-launch.c` — the maintainer-script interpreter, as a real ELF binary
  (not a shell script — the kernel follows only one `#!` level).
- `dn-run.c` — the runtime launch dispatcher: classifies a target's ELF
  at launch and picks the shim, plain exec, or the tracer.
