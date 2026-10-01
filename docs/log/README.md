# docs/log/

Chronological or one-off investigation, not current state. Durable
reference knowledge found along the way gets pulled into `../spec/` with
a pointer left here — see each file's own note.

- `findings.md` — the engineering log (chronological). Append, don't
  rewrite.
- `android-seccomp-audit.md` — the investigation that produced
  [`../spec/android-platform.md`](../spec/android-platform.md) (the gate
  taxonomy, the glibc patch catalog), plus the on-device glibc build
  attempt log.
- `survey-0.2.0.md` (+ `survey-0.2.0/`, its raw data) — the 0.2.0-prealpha
  survey: 100 random Debian packages installed and run in the prefix.
