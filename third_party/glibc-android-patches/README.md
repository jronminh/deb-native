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
- **This patch:** forked from [`termux-pacman/glibc-packages`](https://github.com/termux-pacman/glibc-packages)
  (`gpkg/glibc/`, GPL-2.0+, same license as glibc itself). Per-file fork
  verdict for all 54 loose files there is in `docs/android-seccomp-audit.md`,
  "Full per-file fork verdict" — this patch carries every file marked
  "fork" there. Highlights:
  - `set-dirs.patch`: `@TERMUX_PREFIX@`/`@TERMUX_PREFIX_CLASSICAL@`
    retargeted to this project's single fixed prefix,
    `/data/data/com.termux/files/home/.dn` (deb-native has one prefix, not
    Termux's dual-prefix concept); a handful of the ~66 touched files
    hand-fixed where Debian's own patch series had already changed the
    surrounding context from what `termux-pacman`'s patch (based on a
    different, non-Debian glibc baseline) expected.
  - `fakesyscall.json`'s mechanism (`syscall.c`, `fakesyscall.h`,
    `fakesyscall-base.h`, `syscall.S.patch`, the generated
    `sysdeps/unix/sysv/linux/aarch64/disabled-syscall.h`): only the
    "real substitute" and "honest `ENOSYS`" buckets are carried — the
    `"0"` (`setuid`/`setgid`/... always-succeeds) bucket is excluded, see
    below.
  - `android_passwd_group.c`/`shmem-android.c` and their wiring (`getXXbyYY*`
    family, SysV `shm*`/`msg*`/`sem*`) are now included — both real
    functionality, not a fake (see the audit doc's bucket breakdown).
  - Real, independently-confirmed Android kernel/ABI fixes: `mprotect`'s
    W^X workaround, `sem_open`'s `link()` -> `symlink()` (this project's
    own `dn-translate-deb.sh` hit the same `link()` restriction
    independently), `set_robust_list` dropped from pthread create/fork,
    `kernel-features.h` (no separate `accept`/`recv`/`send` syscalls on
    this arch), older `faccessat`/`fchmodat`/`fstatat64`/`clock_gettime`
    syscall variants, `/dev/std{in,out}` -> `/proc/self/fd/N`.
  - `disable-clone3.patch` is unconditional (toolchain/compat, not
    Android-specific).

## Explicitly not included yet (parked, not forgotten)

**Fake-root-entangled, waiting on that decision** (`docs/android-seccomp-audit.md`,
"Full per-file fork verdict"): the `"0"`-bucket entries in
`fakesyscall.json` (`setuid`/`setgid`/`setreuid`/`setregid`/`setresuid`/
`setresgid`/`setfsuid`/`setfsgid`), `setfsuid.c`, `setfsgid.c`, and the
`set-fakesyscalls.patch` hunks touching `setegid.c`/`seteuid.c`/`setgid.c`/
`setregid.c`/`setresgid.c`/`setresuid.c`/`setreuid.c`/`setuid.c`/
`local-setxid.h` — these unconditionally fake `set*id` success, the same
shape as `native/path-redirect.c`'s own fake-root mechanism, so forking
them now would reinstate that behavior at the glibc layer independent of
whatever this project decides fake-root's future is. Split out of
`gpkg/glibc/set-fakesyscalls.patch` and kept here, unapplied, as
[`set-fakesyscalls-parked.patch`](set-fakesyscalls-parked.patch) — apply it
on top of `dn-glibc-android.patch` (and add the `"0"`-bucket entries back
into the `jq 'del(.["0"])'` filter below) once fake-root's fate is decided,
rather than re-deriving the split from upstream again.

**Needs its own read before deciding, not a simple fork/skip**:
`set-ld-variables.patch` (a parallel `GLIBC_LD_*` env-var namespace,
checked before the plain `LD_*` names — lands on top of `native/ld-dn.c`'s
own env-building job).

**Deferred, real feature but not urgent for the Alpha goal**: `locale-gen`,
`locale.gen.txt`, `syslog.c` (routes to Android's real `logd`).

**Not applicable**: `i386-syscalls.list.patch`, `glibc32.subpackage.sh`
(this project is arm64-only).

## Applying this patch

```sh
# 1. Base + Debian's own patches
curl -sLO https://deb.debian.org/debian/pool/main/g/glibc/glibc_2.41.orig.tar.xz
curl -sLO https://deb.debian.org/debian/pool/main/g/glibc/glibc_2.41-12+deb13u4.debian.tar.xz
tar xf glibc_2.41.orig.tar.xz && mv glibc-2.41 pristine
tar xf glibc_2.41-12+deb13u4.debian.tar.xz -C pristine
( cd pristine && QUILT_PATCHES=debian/patches quilt push -a )
cp -r pristine work

# 2. Apply this patch -- new files (the fakesyscall.json substitutes,
# android_passwd_group.c, shmem-android.c, the generated
# disabled-syscall.h, ...) are part of the diff (as new-file hunks
# against /dev/null) and land automatically; nothing to copy in by hand.
patch -p1 -d work < dn-glibc-android.patch
```

`work/` is then ready for `configure`. This is exactly what
`.github/workflows/build-glibc.yml` does.

## Regenerating this patch (e.g. against a newer glibc/Termux-patch version)

This patch is `diff -ruN` between an unmodified `pristine/` (step 1 above,
kept around) and a `work/` with every change applied — not maintained as a
quilt series. To regenerate after touching `work/` further:

```sh
diff -ruN --exclude='.pc' --exclude='debian' pristine work > dn-glibc-android.patch
```

Round-trip-verify before trusting a regenerated patch (copy `pristine`
fresh, apply the new patch, diff the result against `work` — should be
empty):

```sh
cp -r pristine /tmp/roundtrip && cd /tmp/roundtrip
patch -p1 --batch -i ../dn-glibc-android.patch
diff -rq --exclude='.pc' --exclude='debian' . ../work   # expect nothing
```

Files sourced from `termux-pacman/glibc-packages` (`gpkg/glibc/`) that are
*new* files, not patches against existing glibc source (`mprotect.c`,
`syscall.c`, `fakesyscall*.h`, `fake_epoll_pwait2.c`, `shm{at,ctl,dt,get}.c`,
`android_passwd_group.{c,h}`, `android_system_user_ids.h`, `shmem-android.{c,h}`)
get `cp`'d into `work/` at the paths `gpkg/glibc/build.sh`'s
`termux_step_pre_configure` copies them to (`sysdeps/unix/sysv/linux/`,
`nss/`, `sysvipc/` respectively) before regenerating. `nss/android_ids.h`
is generated by `gpkg/glibc/gen-android-ids.sh` (not hand-written).
`sysdeps/unix/sysv/linux/aarch64/disabled-syscall.h` is generated the same
way `build.sh` does it (`jq`-driven codegen from `fakesyscall.json`,
matching each entry against `arch-syscall.h`'s `__NR_*` defines) but
against a **filtered** copy of `fakesyscall.json` with the `"0"` key
removed (`jq 'del(.["0"])' fakesyscall.json`) -- re-run that filter, not
the stock file, if `fakesyscall.json` itself changes upstream, or the
fake-root-entangled bucket comes back in silently.
