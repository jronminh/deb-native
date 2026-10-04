# docs/log/

<!-- template: templates/readme.template.md -->

Chronological or one-off investigation, not current state. Durable
reference knowledge found along the way gets pulled into `../spec/` with
a pointer left here — see each file's own note.

- [`findings/`](findings/README.md) — the engineering log, one entry
  per file (split 2026-10-02 from the former single `findings.md`, for
  searchability and a per-entry repo-impact label). Add a new file,
  don't rewrite an existing one.
- `android-seccomp-audit.md` — the investigation that produced
  [`../reference/android-platform.md`](../reference/android-platform.md) (the gate
  taxonomy, the glibc patch catalog), plus the on-device glibc build
  attempt log.
- `ld-dn-runtime.md` + `ld-dn-config.md` — the retired interpreter
  trampoline `native/ld-dn.c` (replaced by the prefix's own fused glibc
  loader in 0.6.0+s.1): its execution phases and its prefix config policy.
  Moved here from `../spec/` when `ld-dn` was retired.
- `survey-0.2.0.md` (+ `survey-0.2.0/`, its raw data) — the 0.2.0-prealpha
  survey: 100 random Debian packages installed and run in the prefix.
