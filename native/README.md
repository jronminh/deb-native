# native/

> Template: [`templates/readme.template.md`](../templates/readme.template.md). Read this
> file before touching anything in this directory or guessing a
> file's purpose from its name alone. Add a `README.md` like this one
> whenever a new directory holds more than a couple of files that
> aren't self-explanatory from their names alone.

The small set of C programs translated Debian programs actually run
through. Built by `scripts/install/setup-runtime.sh` into each prefix;
design background in [`../docs/spec/path-shim.md`](../docs/spec/path-shim.md)
and [`../docs/spec/design.md`](../docs/spec/design.md).

- `ld-dn.c` — the program loader stub: every Debian program in the prefix
  names this as its ELF interpreter (`PT_INTERP`), so the kernel runs it
  first, sets up the shim, and hands over to glibc's real loader. Its
  policy (loader, library dirs, preloads, env, per-program overrides)
  comes from `ld-dn.conf`, installed to
  `$PREFIX/etc/deb-native/ld-dn.conf` — data, so extending it needs no
  rebuild ([`../docs/spec/ld-dn-config.md`](../docs/spec/ld-dn-config.md)).
- `ld-dn.conf` — the loader's default policy file (copied into a prefix by
  `setup-runtime.sh`; compiled defaults stand in when it is absent).
- `path-redirect.c` — the shim: an `LD_PRELOAD` library that rewrites
  `/usr /etc /var /opt /root` into the prefix for glibc dynamic binaries.
  A Bionic build of the same idea for maintainer scripts was tried and
  abandoned — removed from the tree; see
  [`../docs/spec/path-shim.md`](../docs/spec/path-shim.md), "Dead end,
  fully explored" for the record.
- `dn-launch.c` — the maintainer-script interpreter, as a real ELF binary
  (not a shell script — the kernel follows only one `#!` level).
- `dn-run.c` — the runtime launch dispatcher: classifies a target's ELF
  at launch and picks the shim, plain exec, or the tracer.
