# docs/spec/

<!-- template: templates/readme.template.md -->

The project's own design: what it does and how, as it stands right now.
Read these as ground truth; update them when the design changes. Facts
about the platform, ABI and formats the project leans on live in
[`../reference/`](../reference/README.md); comparisons and superseded
designs live in [`../notes/`](../notes/README.md).

- `status.md` — what works today, the scope it holds to, the proof, and what
  is not built yet.
- `design.md` — the live design: scope, the self-contained prefix,
  day-to-day commands, fake root. Accumulated over releases; points out
  to the files here and below for depth.
- `dn-glibc-prefix.md` — the prefix's glibc: its own loader built for the
  prefix, and the packaging + GCC lifecycle that make it installable.
- `install-flow.md` — the bootstrap/install order, end to end.
- `package-lifecycle.md` — one package's lifecycle, stage by stage, mapped
  to the hook/script/layer that handles each (`install-flow.md` is the
  one-time bootstrap instead).
- `userlands.md` — one host (Android) and its sibling userlands (Termux's
  and the project's) and the interface over them: the Debian userland is
  the default session; `termux-shell` crosses to Termux, `dn-shell` back;
  distinct prompts and a `pkg` guard.
- `deploy.md` — how a `dn-glibc` prefix is deployed: install Debian's real
  `libc6`/`libc-bin`, then swap in the patched files; the rest of the
  bootstrap is unchanged.

Subdirectories, each with its own index:

- [`shim/`](shim/README.md) — the path-redirect shim and what it does not
  cover.
- [`tracer/`](tracer/README.md) — the syscall tracer (`dn-trace`) and its
  bind-only path.
