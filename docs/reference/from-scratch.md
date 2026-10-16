# Building a tree from scratch: LFS and its neighbours

A variant is a tree ([`../spec/variants.md`](../spec/variants.md)). Its packages
can come from Debian's `.deb`s -- the reference, and where the builder's
fingerprints come from -- or be built from source. When the goal is a tree that
names no distro, or one smaller or newer than Debian's, the references are these.
None of them builds the *kernel*: the kernel here is the phone's, mapped by the
core, so the half of LFS and Buildroot that builds a kernel does not apply.

## Linux From Scratch (LFS)

The book that builds a Linux system from source, step by step
([linuxfromscratch.org](https://www.linuxfromscratch.org/lfs/)). Its shape is
what matters:

- **The toolchain bootstrap.** Build `binutils`, then a bootstrap `gcc` and
  `glibc`, into a scratch prefix (LFS uses `/tools`), chroot in and build the
  *real* toolchain, then rebuild the temporary tools with it -- the chicken-and-
  egg solved by one throwaway toolchain. This is the recipe to take if a tree is
  ever to carry its *own* glibc instead of Debian's (and drop the `Debian GLIBC
  …` fingerprint the core still shows, [`../spec/variants.md`](../spec/variants.md),
  "What still names Debian").
- **Build in a scratch area, then produce the image.** The same split this
  project calls **builder vs image**.
- < 500 MB by the book, smaller for one job (the book cites an Apache-only
  system under 200 MB); fully auditable, since it is all source.

**A recursion:** LFS needs a working Linux host with a toolchain. Here that host
can be a **deb-native prefix** -- build the LFS tree from inside Debian-on-Android.

## Beyond Linux From Scratch (BLFS)

The companion catalog of packages past the base: servers, the X stack, libraries,
dev tools ([linuxfromscratch.org/blfs](https://www.linuxfromscratch.org/blfs/)).
A source for the `server`, `compute` and `gui` variant sets when a tree is built
by source rather than by `apt`.

## Automated LFS (ALFS) and the hints

Tools and scripts that automate the book, and the *hints* collection for the odd
problem. A model for scripting the builder, though Buildroot and mkroot do more.

## Buildroot

The automated version of the same idea
([buildroot.org](https://buildroot.org/)): a cross-toolchain, a root filesystem
and thousands of packages, chosen from a `menuconfig`, built in 15-30 minutes.
The closest existing "variant builder" -- and the place to look for how a source
build is organized as data. Its kernel and bootloader half is not ours.

## mkroot and toybox

The one-script extreme: **toybox** (Rob Landley's BSD-licensed busybox cousin)
and **mkroot**, a script that builds a small bootable root filesystem. The
spirit of a `build-busybox.sh`: a tiny tree from a short recipe. (Aboriginal
Linux is its ancestor.)

## What to take

- **A source builder** (LFS's bootstrap, or Buildroot's data) as an alternative
  to Debian's `.deb`s, for a tree that names no distro or is smaller than
  Debian's packages allow.
- **Package selection as data** (LFS's package list, BLFS's catalog) for the
  `--set` definitions.
- The **builder/image split** the book makes explicit, which this project already
  uses.

What not to take: the kernel and the bootloader -- the tree does not own the
machine, the host kernel does.
