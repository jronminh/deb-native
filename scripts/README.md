# scripts/

Grouped by where each script sits in a prefix's lifecycle, plus three
standalone categories. When adding a new script, drop it into the
matching directory below rather than back at `scripts/` top level.

- **bootstrap/** — build the prefix itself, once: `setup-apt-prefix.sh`
  (the main entry point), `bootstrap-base.sh` (0.1.x, inactive),
  `dn-standins.sh`, `dn-package-glibc.sh`, `build-path-redirect.sh`.
- **install/** — runs on every package install, via apt hooks or
  directly: `apt-hook-pre.sh`/`apt-hook-post.sh`, `apt-install.sh`,
  `dn-hook-pre.sh`/`dn-hook-post.sh`, `dn-translate-deb.sh`,
  `dn-debian-index.sh`, `patch-deb.sh`, `patch-elfs.sh`,
  `patch-maintainer-scripts.sh`, `patch-scripts-tree.sh`,
  `normalize-symlinks.sh`, `dn-fix-alternatives.sh`, `native-seed.sh`,
  `setup-runtime.sh`.
- **runtime/** — front end used after a prefix exists: `dn-activate.sh`,
  `dn-adopt.sh`, `make-launchers.sh`, `make-apt-wrappers.sh`.
- **tools/** — standalone diagnostics: `dn-doctor.sh`.
- **bench/** — benchmarking and syscall/symbol scanning, not part of any
  install path: `bench-tracer.sh`, `perf-run.sh`, `scan-libc-symbols.sh`,
  `scan-direct-syscalls.py`.
- **survey/** — compatibility surveys and the 0.1.x prototype pipeline,
  run manually against package samples: `survey.sh`, `survey-apt.sh`,
  `survey-prefix.sh`, `survey-sample.py`, `sample-packages.py`,
  `scope-sample.py`, `prototype-install.sh`.

Scripts reference each other across these directories with
`$HERE/../<category>/<script>.sh` (`$HERE` = the calling script's own
directory), not by name alone — keep that pattern when moving or renaming
a script.
