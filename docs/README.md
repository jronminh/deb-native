# docs/

Split by what kind of writing a file is, not by topic. When adding a new
doc, put it in the directory that matches what it *is*, not where it feels
thematically closest.

- **spec/** — technical specification: what the project does and how, as
  it stands right now. Read these as ground truth; update them when the
  design changes. Includes `design.md` (one file, accumulated over
  releases — see its own note), `standard.md`,
  `multiarch-mechanics.md`, `syscall-boundary.md`, `shim-coverage.md`
  (+ `coverage/`, its measured data), `tracer.md`, `install-flow.md`,
  `runtime-failures.md`, `bind-only.md`, `vs-sudo-less.md`,
  `direct-usage.md`.
- **log/** — chronological or one-off investigation, not current state:
  `findings.md` (the engineering log — append, don't rewrite),
  `android-seccomp-audit.md` (an audit's findings), `survey-0.2.0.md`
  (+ `survey-0.2.0/`, one survey run's raw data).
- **guides/** — how-to for a specific, one-off case, not a spec of the
  project itself: `tailscale.md`.

A spec doc that gets superseded moves to `log/` (or gets a status note)
rather than being silently edited into agreement with history it never
described.
