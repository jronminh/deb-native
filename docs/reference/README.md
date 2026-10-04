# docs/reference/

<!-- template: templates/readme.template.md -->

Durable look-up material: facts about the platform, ABI and formats the
project leans on, plus the support contract and the known-issues catalog.
Unlike [`../spec/`](../spec/README.md), these are not the project's own
design — they change when the environment or our understanding changes,
not when the design changes.

- `android-platform.md` — the Android enforcement-gate taxonomy
  (seccomp/capability/SELinux) and the glibc patch's per-file verdict.
- `syscall-boundary.md` — what libc interposition cannot see at all.
- `dl-mechanics.md` — catalog of glibc's dynamic-linker mechanisms (env,
  files, CLI, dynamic tags, tunables, audit).
- `multiarch-mechanics.md` — dpkg multi-arch mechanics as the prefix's
  apt/dpkg uses them.
- `elf-interp-patch.md` — the one on-disk ELF edit the project makes
  (`PT_INTERP`): the fields touched, why the string usually has to move.
- `standard.md` — package scope: what "supported" means and how a claim
  about a package is written down and proved.
- `known-issues.md` — the confirmed breakages in the current tree, each
  with a reproduction and its root cause.
