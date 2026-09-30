# dn-glibc-android.patch

0.5.0's "own glibc" milestone 1 (see `TODO.md`'s 0.5.0 roadmap and
`docs/android-seccomp-audit.md`'s "Termux's actual Android patch series"
section for the full history). One combined, ready-to-apply patch that
turns Debian's real, unmodified `glibc` source into a build that runs under
deb-native's fixed prefix (`/data/data/com.termux/files/home/.dn`).

## What it is

`diff -ruN` between:

- **Base:** Debian's `glibc` source package `2.41-12+deb13u4`
  (`glibc_2.41.orig.tar.xz` + `glibc_2.41-12+deb13u4.debian.tar.xz` from
  `deb.debian.org`), with Debian's own ~80-patch `debian/patches/series`
  already applied via `quilt push -a`. This step is **required** and not
  optional: real Debian glibc's `configure`/build assumes it (confirmed by
  a direct on-device test, `android-seccomp-audit.md`).
- **This patch:** forked from two files in
  [`termux-pacman/glibc-packages`](https://github.com/termux-pacman/glibc-packages)
  (`gpkg/glibc/set-dirs.patch`, `gpkg/glibc/disable-clone3.patch`, GPL-2.0+,
  same license as glibc itself), with:
  - `@TERMUX_PREFIX@` and `@TERMUX_PREFIX_CLASSICAL@` both retargeted to
    this project's single fixed prefix, `/data/data/com.termux/files/home/.dn`
    (deb-native has one prefix, not Termux's dual-prefix concept);
  - 4 of the ~66 touched files hand-fixed where Debian's own patch series
    had already changed the surrounding context from what
    `termux-pacman`'s patch (based on a different, non-Debian glibc
    baseline) expected: `sysdeps/unix/sysv/linux/paths.h`,
    `sysdeps/generic/paths.h`, `nscd/nscd.h` (Debian moved
    `_PATH_VARDB`/nscd's db path off the upstream defaults) and
    `elf/tst-env-setuid.c` (skipped — the line it targets no longer exists
    in glibc 2.41's test).

`disable-clone3.patch` is unconditional (toolchain/compat, not
Android-specific): disables `clone3` use, applies cleanly with fuzz.
`set-dirs.patch` is the load-bearing NSS/path fix: every hardcoded
`/etc`, `/var`, `/tmp`, `/usr/...` path glibc's own source reads at
runtime is prefixed to the fixed path above, so NSS resolves the prefix's
own `/etc/passwd` directly — no tracer route needed for that case.

## Explicitly not included yet (see TODO.md, "what it needs")

`fakesyscall.json` (both buckets — the `"0"` set*id-always-succeeds bucket
is entangled with fake-root's undecided fate), `android_passwd_group.c`,
`shmem-android.c`, the smaller unread `termux-pacman` patches. Decide these
after this milestone's build proof lands, not before.

## Reproducing / regenerating this patch

```sh
# 1. Base + Debian's own patches
curl -sLO https://deb.debian.org/debian/pool/main/g/glibc/glibc_2.41.orig.tar.xz
curl -sLO https://deb.debian.org/debian/pool/main/g/glibc/glibc_2.41-12+deb13u4.debian.tar.xz
tar xf glibc_2.41.orig.tar.xz && mv glibc-2.41 pristine
tar xf glibc_2.41-12+deb13u4.debian.tar.xz -C pristine
( cd pristine && QUILT_PATCHES=debian/patches quilt push -a )
cp -r pristine work

# 2. Apply this patch
patch -p1 -d work < dn-glibc-android.patch
```

`work/` is then ready for `configure`.
