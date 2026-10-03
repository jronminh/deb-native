# dn-glibc-android.patch

> Template: [`templates/readme.template.md`](../../templates/readme.template.md). Read this
> file before touching anything in this directory or guessing a
> file's purpose from its name alone. Add a `README.md` like this one
> whenever a new directory holds more than a couple of files that
> aren't self-explanatory from their names alone.

0.5.0's "own glibc" milestone 1 (see `TODO.md`'s 0.5.0 roadmap and
`docs/spec/android-platform.md`'s "Termux's Android glibc patch: catalog
and fork verdict" section for the catalog; `docs/log/android-seccomp-audit.md`
has the full investigation history). One combined, ready-to-apply patch that
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
  verdict for all 54 loose files there is in `docs/spec/android-platform.md`,
  "Per-file verdict, everything in `gpkg/glibc/`" — this patch carries every file marked
  "fork" there. Highlights:
  - `set-dirs.patch`: `@TERMUX_PREFIX_CLASSICAL@` resolved to the same
    thing as `@TERMUX_PREFIX@` (deb-native has one prefix, not Termux's
    dual-prefix concept); `@TERMUX_PREFIX@` itself is kept as a literal
    placeholder in this checked-in patch, substituted for a real,
    absolute prefix only when applying
    (`dn-apply-glibc-patch.sh`, "Applying this patch" below) -- not baked
    in, so the same patch can target a throwaway test prefix instead of
    `/data/data/com.termux/files/home/.dn`. A handful of the ~66 touched
    files were hand-fixed where Debian's own patch series had already
    changed the surrounding context from what `termux-pacman`'s patch
    (based on a different, non-Debian glibc baseline) expected.
  - `elf/rtld.c`: ignore the inherited `LD_PRELOAD` (skip the
    `state.preloadlist` source in `dl_main`; `--preload` and the
    `ld.so.preload` file are kept). deb-native's own addition, not Termux's:
    the fused loader delivers the path-redirect shim via `ld.so.preload`
    instead of the env, and a host `LD_PRELOAD` (Termux's
    `libtermux-exec-ld-preload.so`) is built for another glibc and would
    otherwise abort every prefix program at startup
    (`docs/spec/dn-glibc-prefix.md`).
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
  - `sysdeps/unix/sysv/linux/aarch64/clone3.S` (aarch64's raw `clone3()`
    syscall stub) is deleted as a diff hunk against `/dev/null`, the same
    way `gpkg/glibc/build.sh` does it unconditionally as its first step
    (`termux_step_pre_configure`). Needed because `clone3` is in
    `fakesyscall.json`'s `ENOSYS` bucket, and the generated
    `disabled-syscall.h` deletes `__NR_clone3` from `arch-syscall.h` so
    nothing can reach the real (Gate-A-killed) syscall by accident — but
    this raw `.S` stub referenced `__NR_clone3` directly, outside the
    fakesyscall dispatch path, so it fails to assemble unless deleted.
    Found and fixed during the first on-device build
    (`docs/log/android-seccomp-audit.md`, 2026-09-30), initially as a manual
    tree edit; folded into this patch (as opposed to `work/`'s hand-fix)
    after a clean-room replay (fresh `pristine` + this patch alone, no
    other hand-fixes) confirmed it was still missing.

## Explicitly not included yet (parked, not forgotten)

**Fake-root-entangled, waiting on that decision** (`docs/spec/android-platform.md`,
"Per-file verdict, everything in `gpkg/glibc/`"): the `"0"`-bucket entries in
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

The patch itself names no prefix: every path this build must know at
compile time (`ld.so.preload`, the guest `/etc`, ...) is the placeholder
`@TERMUX_PREFIX@`, substituted for a real, absolute, no-trailing-slash
prefix only when applying -- so the same patch targets `.dn` (production)
or a throwaway test prefix (`docs/spec/dn-glibc-prefix.md`, "Install
order") without being hand-edited or re-generated.

```sh
# 1. Base + Debian's own patches
curl -sLO https://deb.debian.org/debian/pool/main/g/glibc/glibc_2.41.orig.tar.xz
curl -sLO https://deb.debian.org/debian/pool/main/g/glibc/glibc_2.41-12+deb13u4.debian.tar.xz
tar xf glibc_2.41.orig.tar.xz && mv glibc-2.41 pristine
tar xf glibc_2.41-12+deb13u4.debian.tar.xz -C pristine
( cd pristine && QUILT_PATCHES=debian/patches quilt push -a )
cp -r pristine work

# 2. Apply this patch, substituting the target prefix for @TERMUX_PREFIX@
# (../../scripts/bootstrap/dn-apply-glibc-patch.sh) -- new files (the
# fakesyscall.json substitutes, android_passwd_group.c, shmem-android.c,
# the generated disabled-syscall.h, ...) are part of the diff (as new-file
# hunks against /dev/null) and land automatically; nothing to copy in by
# hand.
../../scripts/bootstrap/dn-apply-glibc-patch.sh work /data/data/com.termux/files/home/.dn
```

`work/` is then ready for `configure --prefix=<the same prefix>/usr`. This
is exactly what `.github/workflows/build-glibc.yml` does.

## Regenerating this patch (e.g. against a newer glibc/Termux-patch version)

This patch is `diff -ruN` between an unmodified `pristine/` (step 1 above,
kept around) and a `work/` with every change applied — not maintained as a
quilt series. `work/` has a real prefix baked in (whatever
`dn-apply-glibc-patch.sh` substituted), so regenerating must turn that back
into the placeholder before it is trusted as the checked-in patch:

```sh
diff -ruN --exclude='.pc' --exclude='debian' pristine work \
  | sed "s|/data/data/com.termux/files/home/.dn|@TERMUX_PREFIX@|g" \
  > dn-glibc-android.patch
```

(replace `/data/data/com.termux/files/home/.dn` with whatever prefix that
`work/` was actually built for, if not `.dn`.)

Round-trip-verify before trusting a regenerated patch (copy `pristine`
fresh, apply the new patch for the same prefix, diff the result against
`work` — should be empty):

```sh
cp -r pristine /tmp/roundtrip
../../scripts/bootstrap/dn-apply-glibc-patch.sh /tmp/roundtrip /data/data/com.termux/files/home/.dn
diff -rq --exclude='.pc' --exclude='debian' /tmp/roundtrip work   # expect nothing
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

## Building and packaging (validated 2026-09-30)

On-device, against a fresh Debian glibc source (`2.41-12+deb13u4`) with
this patch applied (`patch -p1`, no hand-fixes -- clean-room verified):

```sh
cc1_gcc=/data/data/com.termux/files/home/.dn/usr/bin/gcc-14
KHEADERS=<merged linux-libc-dev headers, --with-headers target>
PATH=/data/data/com.termux/files/usr/bin \
  CC="$cc1_gcc" ../work/configure \
  --prefix=/data/data/com.termux/files/home/.dn/usr \
  --libdir=/data/data/com.termux/files/home/.dn/usr/lib \
  --includedir=/data/data/com.termux/files/home/.dn/usr/include \
  --host=aarch64-linux-gnu --build=aarch64-linux-gnu \
  --with-headers="$KHEADERS" \
  --enable-memory-tagging --enable-fortify-source --enable-bind-now \
  --disable-multi-arch --enable-stack-protector=strong --disable-nscd \
  --disable-profile --disable-werror --disable-default-pie
PATH=/data/data/com.termux/files/usr/bin make -O -j8   # NOT -j1: $DN must
  # be off PATH (COMPILER_PATH from ld-dn.c is sufficient) -- with $DN on
  # PATH, this project's own coreutils going through ld-dn+shim under
  # heavy repeated invocation was unstable (docs/log/findings/); -j8 itself
  # is not the issue and is ~3-4x faster than -j1.
make -k install DESTDIR=<destdir>   # -k: the manual subdir fails for an
  # unrelated missing-texinfo-source reason, nothing else is affected
```

`-disable-multi-arch` means `make install` lays libraries out flat under
`$DESTDIR/usr/lib/`, not Debian's `usr/lib/aarch64-linux-gnu/`. Confirmed
safe to relocate at packaging time rather than rebuild: no binary or
cache file (`libc.so.6`, `gconv-modules.cache`) has that path baked in as
a literal string (checked with `strings`), and `native/ld-dn.c` already
sets `LD_LIBRARY_PATH` covering both locations per launch regardless.

**Packaging as `libc6`**: [`scripts/bootstrap/dn-package-glibc.sh`](../../scripts/bootstrap/dn-package-glibc.sh)
takes a real Debian `libc6_<ver>_arm64.deb` (`apt-get download
libc6=<ver>`, matching version) as a template -- reusing Debian's own
maintainer scripts/triggers/symbols/doc rather than reinventing them --
and replaces only the shared-library payload with this build's own,
relocated into the multiarch directory, plus a version bump
(`<ver>+dn1`). One file needs generating first, not part of `make
install`'s own output: `gconv-modules.cache` (a fastload cache, built by
`iconvconfig`, itself part of this project's own build output under
`usr/sbin/`):

```sh
objdir/elf/ld.so --library-path "$DESTDIR/usr/lib" \
  "$DESTDIR/usr/sbin/iconvconfig" --nostdlib \
  -o "$DESTDIR/usr/lib/gconv/gconv-modules.cache" "$DESTDIR/usr/lib/gconv"

scripts/bootstrap/dn-package-glibc.sh libc6_<ver>_arm64.deb "$DESTDIR" out.deb
dpkg -i out.deb   # not apt-get -- see TODO.md's Runtime component audit
                   # for why apt-get's own hook pipeline needed separate
                   # fixing; dpkg -i is the lower-risk path regardless
echo "libc6:arm64 hold" | dpkg --set-selections   # persists across dpkg -i
                   # if not already held by dn-standins.sh's stand-in --
                   # a real Debian libc6 pulled in by a later `apt upgrade`
                   # segfaults at startup (android-seccomp-audit.md)
```

Verified end to end (2026-09-30): installs clean over the previous
stand-in (`dn-standins.sh`'s Termux-glibc symlinks), `ls -l` resolves
real NSS identities, previously-installed packages (`tree`, `figlet`,
...) keep running, and the full regression battery from the runtime
component audit (`find -exec test`, a fresh `apt-get install`) stays
clean. **`libc-bin` packaged 2026-10-03**:
[`scripts/bootstrap/dn-package-libc-bin.sh`](../../scripts/bootstrap/dn-package-libc-bin.sh)
is the companion to `dn-package-glibc.sh` (below) -- it takes a real Debian
`libc-bin_<ver>_arm64.deb` as a template and swaps in this build's own
`usr/bin`/`usr/sbin` programs (`ldconfig`, `ldd`, `getconf`, `locale`, ...).
Unlike `libc6-dev`/`libc-dev-bin` (version-only, install unmodified once
`libc6`'s version matches -- `libc6-dev-gap-closed.md`), `libc-bin` is
*path*-sensitive: its `ldconfig` is compiled against `SYSCONFDIR` and must be
this build's to write `<prefix>/usr/etc/ld.so.cache`
(`docs/log/findings/own-glibc-missing-libc-bin.md`). Remaining out of scope:
`libc6-dev` (headers/static libs, no script needed) and
`libc-l10n`/`locales` (still pinned, untested).
