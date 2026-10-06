# core/install/

<!-- template: templates/readme.template.md -->

Runs on every package install, via the prefix's own apt hooks or
directly. The classic (pre-0.2.0) design's apt hooks
(`apt-hook-pre.sh`/`apt-hook-post.sh`), their maintainer-script patcher
(`patch-maintainer-scripts.sh`), the per-step ELF/control patchers they
called (`patch-deb.sh`, `patch-elfs.sh`), and the stub-database dependency
seeder (`native-seed.sh`, replaced by the real `libc6` stand-in) are gone
— see [`../../docs/notes/classic-design.md`](../../docs/notes/classic-design.md)
and [`../../docs/notes/native-reuse.md`](../../docs/notes/native-reuse.md)
for the record. See [`../../docs/spec/design.md`](../../docs/spec/design.md)
(the install pipeline) and [`../../docs/spec/install-flow.md`](../../docs/spec/install-flow.md)
for the current one.

- `apt-install.sh` — installs packages (and dependencies) into a prefix
  via plain `apt`, letting apt's own hooks do the translation.
- `dn-hook-pre.sh` / `dn-hook-post.sh` — the prefix's own
  `DPkg::Pre-Install-Pkgs` / `DPkg::Post-Invoke` hooks: translate each
  `.deb` before dpkg sees it, then fix up alternatives/symlinks/launchers
  after.
- `dn-translate-deb.sh` — translates one `.deb` in place: ELF interpreter,
  library path, `#!` lines, maintainer scripts, hard links, per-package
  fixes from `core/custom/`.
- `dn-debian-index.sh` — rewrites `Architecture: all` -> `arm64` in a
  downloaded Debian package index.
- `setup-runtime.sh` — builds and installs the maintainer-script runtime
  (`dn-run`, the shim, the tracer) inside a prefix.
