# docs/spec/

> Template: [`templates/readme.template.md`](../../templates/readme.template.md). Read this
> file before touching anything in this directory or guessing a
> file's purpose from its name alone. Add a `README.md` like this one
> whenever a new directory holds more than a couple of files that
> aren't self-explanatory from their names alone.

Technical specification: what the project does and how, as it stands
right now. Read these as ground truth; update them when the design
changes.

- `design.md` — the live design: scope, the 0.2.0 self-contained prefix
  (what's actually built), day-to-day commands, fake root. One doc,
  accumulated over releases; points out to the files below for depth or
  history.
- `path-shim.md` — the path-redirect shim: design, verification, and the
  ways a glibc target can be made to load it.
- `native-reuse.md` — native dependency reuse (`native-seed.sh`): how a
  Debian dependency is matched against what Termux's `*-glibc` already has.
- `classic-design.md` — the pre-0.2.0 approach (plain `dpkg --instdir`,
  static per-binary wrappers, the services research) — superseded in
  large part by the 0.2.0 pivot, kept as the record.
- `prior-art.md` — sudo-less and proroot: what carries over, what doesn't.
- `vs-sudo-less.md` — the structured, per-concern diff against sudo-less.
- `alternatives.md` — comparison with the other ways to run Debian on
  Android (chroot, proot-distro, namespaces, ...).
- `standard.md` — package scope: what "supported" means and how a claim
  about a package is written down and proved.
- `install-flow.md` — the bootstrap/install order, end to end.
- `multiarch-mechanics.md` — dpkg multi-arch mechanics, shared with the
  `naibed` branch.
- `path-shim.md` is the mechanism; the boundary around it:
  - `shim-coverage.md` — which libc entry points the shim covers (+
    `coverage/`, the measured corpus data).
  - `syscall-boundary.md` — what libc interposition cannot see at all.
  - `direct-usage.md` — living investigation into what bypasses the shim.
  - `bind-only.md` — the tracer's bind-only path fast path.
  - `tracer.md` — the syscall tracer (`dn-trace`) itself.
- `runtime-failures.md` — what goes wrong when *running* a program,
  grouped by cause.
- `android-platform.md` — the Android enforcement-gate taxonomy (seccomp/
  capability/SELinux) and the glibc patch's per-file fork verdict.
