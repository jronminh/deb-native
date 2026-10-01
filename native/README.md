# native/

The small set of C programs translated Debian programs actually run
through. Built by `scripts/install/setup-runtime.sh` into each prefix;
design background in [`../docs/spec/path-shim.md`](../docs/spec/path-shim.md)
and [`../docs/spec/design.md`](../docs/spec/design.md).

- `ld-dn.c` — the program loader stub: every Debian program in the prefix
  names this as its ELF interpreter (`PT_INTERP`), so the kernel runs it
  first, sets up the shim, and hands over to glibc's real loader.
- `path-redirect.c` — the shim: an `LD_PRELOAD` library that rewrites
  `/usr /etc /var /opt /root` into the prefix for glibc dynamic binaries.
- `path-redirect-bionic.c` — a Bionic build of the same idea for
  maintainer scripts; a dead end, kept for the record (see
  [`../docs/spec/path-shim.md`](../docs/spec/path-shim.md), "Dead end,
  fully explored").
- `dn-launch.c` — the maintainer-script interpreter, as a real ELF binary
  (not a shell script — the kernel follows only one `#!` level).
- `dn-run.c` — the runtime launch dispatcher: classifies a target's ELF
  at launch and picks the shim, plain exec, or the tracer.
