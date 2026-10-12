# patches/

<!-- template: templates/readme.template.md -->

The patches this project carried for its **glibc fork**. On branch
`dn-interp-surgical` none of them is applied, and one is gone entirely.

- **`dn-glibc-android.patch` — retired, removed from the tree.** This project
  builds no glibc and patches none: the tree takes Debian's own **stock**
  `libc6`/`libc-bin` from the mirror, and the syscalls Android denies
  (`rseq`, `clone3`) are answered at the *syscall boundary* by `dn-trace`, not
  by a patched glibc. The direct device evidence is in
  [`docs/reference/android-platform.md`](../docs/reference/android-platform.md)
  ("Stock `libc6` re-tested"); the hunk-by-hunk trim is in
  [`docs/spec/dn-interp-shim.md`](../docs/spec/dn-interp-shim.md) ("A2"). The
  file survives in `main`'s history if the fork is ever revisited.
- **`dn-policy-glibc-wiring.patch` — not applied on this branch.** The fast
  path is the shim (`dn-shim.so`), and the runtime no longer lives inside
  glibc. Kept in the tree for reference while the shim adoption finishes.
- **`set-fakesyscalls-parked.patch` — parked, not applied.**

Re-adopting any of these means rebuilding the glibc fork this branch
retired — read the branch design doc
([`docs/spec/dn-interp-shim.md`](../docs/spec/dn-interp-shim.md)) first.
