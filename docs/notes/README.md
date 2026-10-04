# docs/notes/

<!-- template: templates/readme.template.md -->

Comparisons, positioning, and superseded designs — not current state.
Kept because the reasoning or the contrast is still useful; the live
design is in [`../spec/`](../spec/README.md), and the change history is in
[`../log/`](../log/README.md).

- `alternatives.md` — comparison with the other ways to run Debian on
  Android (chroot, proot-distro, namespaces, ...).
- `vs-sudo-less.md` — the structured, per-concern diff against sudo-less.
- `prior-art.md` — sudo-less and proroot: what carries over, what doesn't.
- `native-reuse.md` — native dependency reuse via Termux's `*-glibc`
  packages, before the real `libc6` stand-in made it unnecessary.
- `classic-design.md` — the pre-0.2.0 approach (plain `dpkg --instdir`,
  static per-binary wrappers, the services research).
