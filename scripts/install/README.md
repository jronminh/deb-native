# scripts/install/

> Template: [`templates/readme.template.md`](../../templates/readme.template.md). Read this
> file before touching anything in this directory or guessing a
> file's purpose from its name alone. Add a `README.md` like this one
> whenever a new directory holds more than a couple of files that
> aren't self-explanatory from their names alone.

Runs on every package install, via apt hooks or directly. See
[`../../docs/spec/design.md`](../../docs/spec/design.md) (the install
pipeline) and [`../../docs/spec/install-flow.md`](../../docs/spec/install-flow.md).

- `apt-hook-pre.sh` / `apt-hook-post.sh` — the classic (pre-0.2.0) design's
  apt hooks; see [`../../docs/spec/classic-design.md`](../../docs/spec/classic-design.md).
- `apt-install.sh` — installs packages (and dependencies) into a prefix
  via plain `apt`, letting apt's own hooks do the translation.
- `dn-hook-pre.sh` / `dn-hook-post.sh` — the prefix's own
  `DPkg::Pre-Install-Pkgs` / `DPkg::Post-Invoke` hooks: translate each
  `.deb` before dpkg sees it, then fix up alternatives/symlinks/launchers
  after.
- `dn-translate-deb.sh` — translates one `.deb` in place: ELF interpreter,
  library path, `#!` lines, maintainer scripts, hard links, per-package
  fixes from `custom/`.
- `dn-debian-index.sh` — rewrites `Architecture: all` -> `arm64` in a
  downloaded Debian package index.
- `patch-deb.sh` — patches a `.deb`'s maintainer control scripts before
  dpkg ever sees it (the bare-`dpkg` path's equivalent of the apt hooks).
- `patch-elfs.sh` — repoints every Debian glibc ELF in the prefix at
  Termux's glibc loader (`grun --configure`); the classic design's
  post-install step, since replaced by translating ELFs in the package.
- `patch-maintainer-scripts.sh` — rewrites hardcoded absolute paths in a
  package's maintainer scripts between `--unpack` and `--configure`.
- `patch-scripts-tree.sh` — rewrites an extracted package's maintainer
  scripts' `#!` shebang to the prefix's own interpreter.
- `normalize-symlinks.sh` — rewrites absolute symlinks inside the prefix
  so the kernel resolves them within it (needed for bind-only tracing).
- `dn-fix-alternatives.sh` — makes every `update-alternatives` link in
  the prefix relative instead of absolute.
- `native-seed.sh` — seeds a dpkg admindir with stub entries for Debian
  dependencies already satisfied by a Termux `*-glibc` package.
- `setup-runtime.sh` — builds and installs the maintainer-script runtime
  (`ld-dn`, `dn-run`, the shim, the tracer) inside a prefix.
