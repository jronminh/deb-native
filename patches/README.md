# dn-glibc-android.patch

<!-- template: templates/readme.template.md -->

0.5.0's "own glibc" milestone 1 (see `TODO.md`'s 0.5.0 roadmap and
`docs/reference/android-platform.md`'s "Termux's Android glibc patch: catalog
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
  verdict for all 54 loose files there is in `docs/reference/android-platform.md`,
  "Per-file verdict, everything in `gpkg/glibc/`" — this patch carries every file marked
  "fork" there. Highlights:
  - `set-dirs.patch`: `@TERMUX_PREFIX_CLASSICAL@` resolved to the same
    thing as `@DN_PREFIX@` (deb-native has one prefix, not Termux's
    dual-prefix concept); `@DN_PREFIX@` itself is kept as a literal
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
    the fused loader delivers the dn-shim shim via `ld.so.preload`
    instead of the env, and a host `LD_PRELOAD` (Termux's
    `libtermux-exec-ld-preload.so`) is built for another glibc and would
    otherwise abort every prefix program at startup
    (`docs/spec/overlay.md`).
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

## Explicitly excluded, permanently (not "parked" anymore)

**Fake-root-entangled — decided against, not waiting on anything**
(`docs/reference/android-platform.md`, "Per-file verdict, everything in
`gpkg/glibc/`"): the `"0"`-bucket entries in `fakesyscall.json`
(`setuid`/`setgid`/`setreuid`/`setregid`/`setresuid`/`setresgid`/
`setfsuid`/`setfsgid`), `setfsuid.c`, `setfsgid.c`, and the
`set-fakesyscalls.patch` hunks touching `setegid.c`/`seteuid.c`/`setgid.c`/
`setregid.c`/`setresgid.c`/`setresuid.c`/`setreuid.c`/`setuid.c`/
`local-setxid.h` — these unconditionally fake `set*id` success at the
glibc-patch layer. This doc used to say "waiting on fake-root's fate";
that fate is now decided (`docs/spec/overlay.md` principle 3): `dn-policy`
is the single source of truth for fake root, called from both the glibc
fast path and the tracer fallback, specifically so the two can never
disagree. A second, independent fake-root at the glibc-patch layer would
reintroduce exactly that disagreement risk, so this bucket is excluded
**permanently**, not pending a decision. Kept split out, unapplied, as
[`set-fakesyscalls-parked.patch`](set-fakesyscalls-parked.patch) for
reference only (the split itself was real work); do not apply it on top
of `dn-glibc-android.patch`, and do not add the `"0"`-bucket entries back
into the `jq 'del(.["0"])'` filter below.

**Needs its own read before deciding, not a simple fork/skip**:
`set-ld-variables.patch` (a parallel `GLIBC_LD_*` env-var namespace,
checked before the plain `LD_*` names — lands on top of the project's own
glibc-child environment).

**Deferred, real feature but not urgent for the Alpha goal**: `locale-gen`,
`locale.gen.txt`, `syslog.c` (routes to Android's real `logd`).

**Not applicable**: `i386-syscalls.list.patch`, `glibc32.subpackage.sh`
(this project is arm64-only).

## Applying this patch

The patch itself names no prefix: every path this build must know at
compile time (`ld.so.preload`, the guest `/etc`, ...) is the placeholder
`@DN_PREFIX@`, substituted for a real, absolute, no-trailing-slash
prefix only when applying -- so the same patch targets `.dn` (production)
or a throwaway test prefix (`docs/spec/overlay.md`, "Install
order") without being hand-edited or re-generated.

```sh
# 1. Base + Debian's own patches
curl -sLO https://deb.debian.org/debian/pool/main/g/glibc/glibc_2.41.orig.tar.xz
curl -sLO https://deb.debian.org/debian/pool/main/g/glibc/glibc_2.41-12+deb13u4.debian.tar.xz
tar xf glibc_2.41.orig.tar.xz && mv glibc-2.41 pristine
tar xf glibc_2.41-12+deb13u4.debian.tar.xz -C pristine
( cd pristine && QUILT_PATCHES=debian/patches quilt push -a )
cp -r pristine work

# 2. Apply this patch, substituting the target prefix for @DN_PREFIX@
# (../../bootstrap/dn-apply-glibc-patch.sh) -- new files (the
# fakesyscall.json substitutes, android_passwd_group.c, shmem-android.c,
# the generated disabled-syscall.h, ...) are part of the diff (as new-file
# hunks against /dev/null) and land automatically; nothing to copy in by
# hand.
../../bootstrap/dn-apply-glibc-patch.sh work /data/data/com.termux/files/home/.dn
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
  | sed "s|/data/data/com.termux/files/home/.dn|@DN_PREFIX@|g" \
  > dn-glibc-android.patch
```

(replace `/data/data/com.termux/files/home/.dn` with whatever prefix that
`work/` was actually built for, if not `.dn`.)

Round-trip-verify before trusting a regenerated patch (copy `pristine`
fresh, apply the new patch for the same prefix, diff the result against
`work` — should be empty):

```sh
cp -r pristine /tmp/roundtrip
../../bootstrap/dn-apply-glibc-patch.sh /tmp/roundtrip /data/data/com.termux/files/home/.dn
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
  # be off PATH (the toolchain's COMPILER_PATH is sufficient) -- with $DN on
  # PATH, this project's own coreutils going through the loader+shim under
  # heavy repeated invocation was unstable (docs/log/findings/); -j8 itself
  # is not the issue and is ~3-4x faster than -j1.
make -k install DESTDIR=<destdir>   # -k: the manual subdir fails for an
  # unrelated missing-texinfo-source reason, nothing else is affected
```

`-disable-multi-arch` means `make install` lays libraries out flat under
`$DESTDIR/usr/lib/`, not Debian's `usr/lib/aarch64-linux-gnu/`. Confirmed
safe to relocate at packaging time rather than rebuild: no binary or
cache file (`libc.so.6`, `gconv-modules.cache`) has that path baked in as
a literal string (checked with `strings`), and the prefix's `ld.so.cache`
covers both locations regardless.

**Packaging as `libc6`**: `bootstrap/dn-package-glibc.sh`
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

bootstrap/dn-package-glibc.sh libc6_<ver>_arm64.deb "$DESTDIR" out.deb
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
`bootstrap/dn-package-libc-bin.sh`
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

# dn-policy-glibc-wiring.patch

The second patch in the dn-glibc build, applied **on top of**
`dn-glibc-android.patch` and kept separate from it. It carries the first
slices of `runtime.md`'s "wire dn-policy into every path-taking function":
the public `open`/`openat` family and the `stat`/`fstatat`/`statx`/`faccessat`
family now call into the real policy built and tested in `src/dn-policy/` --
path translation through `dn_policy_redirect()` (and its `_nofollow` variant),
plus `dn_policy_stat_post()` rewriting a stat result's owner fields from the
owner store (fake root) -- instead of `__dn_redirect`'s inline root
heuristic. It also carries the reverse translation (`getcwd()` and the
`/proc/<self>` magic links), path translation for the simple manipulation
wrappers (`mkdir`, `rmdir`, `rename`/`renameat`/`renameat2`, `symlink`,
`truncate`, `utimensat`/`utimes`/`utime`, `statfs`), the `syscall(2)`
interposition for the path group, fake root's writes (`chown`/`lchown`/
`chmod`/`fchmodat` record into the owner store), hardlinks (`link`/`unlink`
are link2symlink), the loader's mapping of the fixed gate page (`P_GATE`)
and libc issuing its syscalls from it (the non-cancelable path; the
cancellation asm still calls the kernel directly), and the `RT/lib`-first
library search order. The rest of the wiring (the xattr family,
`chdir`/`chroot`, the cancellation asm, the seccomp filter's gate-IP rule) is
still ahead; what is here is what made the first full glibc build with
dn-policy inside it succeed, extended through every group above.

## What it is

`diff -ruN` between `work-after-official-patch/` (glibc source with
`dn-glibc-android.patch` already applied once, per the section above) and
`work/` (that plus the wiring). Forty-three files:

- New, byte-identical copies of `src/dn-policy/` under
  `sysdeps/unix/sysv/linux/`: `dn-policy.{h,c}`, `dn-policy-fakeroot.c`,
  `dn-policy-hardlink.c`, `dn-policy-internal.h`. The checked-in source of
  truth stays `src/`; the patch embeds a snapshot of it.
- New `sysdeps/unix/sysv/linux/dn-policy-glue.c` -- the glibc side only, not
  in `src/`. It derives `TREE` from `__dn_prefix_get()` and `RT` as
  `<TREE>/usr/lib/deb-native` (the directory the other non-dpkg runtime tools
  already use), calls `dn_policy_init()` lazily under `__libc_lock` (`open()`
  can be called from any thread immediately, unlike `__dn_prefix_init()`,
  which runs in `dl_main`), and exposes the entry points the wrappers
  call: `dn_policy_redirect()` (with `__dn_redirect`'s exact signature),
  `dn_policy_redirect_nofollow()`, `dn_policy_stat_post()`,
  `dn_policy_getcwd_post()`, and `dn_policy_readlink_post()`.
- `sysdeps/generic/dn-prefix.h`: declares every `dn_policy_*` entry point
  above under `#if !IS_IN (rtld)`.
- `sysdeps/unix/sysv/linux/Makefile`: adds the four `dn-policy*` objects to
  `sysdep_routines`.
- The eight `open*.c` call sites (`open`, `open64`, `open{,64}_nocancel`,
  `openat`, `openat64`, `openat{,64}_nocancel`): `__dn_redirect` ->
  `dn_policy_redirect`, guarded by `#if !IS_IN (rtld)`.
- The stat/access family: `fstatat64.c` (the single choke point for `stat`,
  `lstat`, `fstatat`, `fstatat64` on aarch64 -- all route through
  `__fstatat64_time64`), `statx.c`, and `faccessat.c`. Each translates an
  absolute guest path through `dn_policy_redirect()`/`_nofollow()` (the
  latter for `AT_SYMLINK_NOFOLLOW`, so `lstat` never resolves its final
  component); a relative path is left to the kernel (its dirfd or cwd is
  already a real path). On a successful stat, `fstatat64.c` also calls
  `dn_policy_stat_post()` so `st_uid`/`st_gid`/the setuid bits come from the
  owner store. This is glibc's side of `runtime.md`'s "path group" for
  stat/access.
- Reverse translation: `readlink.c` translates the input path (nofollow)
  and, only when that input is under `/proc/`, rewrites the returned target
  into the guest path -- this is what makes `/proc/self/{cwd,fd/N,exe,root}`
  read back as tree paths. A plain in-tree symlink's target is already a
  guest path and is left alone; keeping the rewrite `/proc`-only also keeps
  dn-policy's own `readlink()` calls (symlink resolution, hardlink
  bookkeeping) seeing the raw host target. `getcwd.c` rewrites the path the
  kernel returns, on both the syscall path and the generic fallback.
- Path translation for the simple manipulation wrappers, each just the
  translate-then-syscall the above do: `mkdir.c`, `rmdir.c`,
  `rename.c`/`renameat.c`/`renameat2.c` (both paths), `symlink.c` (the
  link's own path only -- the target text is stored verbatim), `truncate64.c`,
  and `utimensat.c` (`__utimensat64_helper`, which `utimes`/`utime` reach
  too). The `*at` variants that `syscalls.list` generates directly
  (`mkdirat`/`unlinkat`/`symlinkat`/`linkat`/`readlinkat`,
  `inotify_add_watch`) and `chdir`/`chroot` are generated from
  `syscalls.list` and have no wrapper to edit; a program that reaches them
  through a *wrapper function* still falls to the `ptrace` tier, but one
  that calls `syscall(2)` directly is covered by the next bullet.
- `sysdeps/unix/sysv/linux/syscall.c`: glibc's `syscall(2)` -- a C function
  here (the Termux fork renamed the raw entry to `syscallS`) -- now calls
  `dn_policy_syscall_args()` before the raw call, which translates the path
  argument(s) of the path-group numbers and honors `O_NOFOLLOW`/
  `AT_SYMLINK_NOFOLLOW` for `openat`/`newfstatat`/`statx`/`faccessat2` (two
  paths for `renameat`/`renameat2`/`linkat`). Numbers the aarch64 headers
  lack (`chown`/`lchown`) are `#ifdef`-guarded, as are the ones the
  fakesyscall bucket already answers (`statx`/`faccessat2`/`fchmodat2`).
- Fake root's writes: `chown.c`/`lchown.c` never call the real syscall --
  they translate the path (following for `chown`, not for `lchown`), `stat`
  it for its `(dev,ino)`, and record the new owner through
  `dn_policy_owner_merge()`. `chmod.c`/`fchmodat.c` do a real `fchmodat` of
  the ordinary bits (`mode & ~07000`) and record only the setuid/setgid
  bits. `dn_policy_owner_merge()` (new, in `dn-policy-fakeroot.c`) merges
  into any existing record instead of clobbering it, so a `chown` does not
  wipe a `chmod`'s bits and vice versa. The `*xattr` family and the public
  `fchownat()` are `syscalls.list`-generated and have no wrapper to edit:
  a `syscall(2)` caller is covered, the wrapper functions fall to `ptrace`.
- Hardlinks: `link.c` calls `dn_policy_link()` (link2symlink -- the content
  moves into a hidden file under `RT/state/links/` and both names become
  symlinks to it with a shared count); `unlink.c` drops that count (removing
  the hidden file at zero) *before* the real unlink. `fstatat`'s
  post-processing (`dn_policy_stat_post()` -> `dn_policy_hardlink_fixup_stat()`)
  makes the managed names report as ordinary regular files with the right
  `st_nlink`, which the runtime check confirms.
- `elf/dl-load.c`: the loader's system search dirs now start with `RT/lib`
  (`/usr/lib/deb-native/lib/`, redirected to `<TREE>/usr/lib/deb-native/lib/`
  like every other dir), then the tree's own `/usr/lib/aarch64-linux-gnu/`
  and `/usr/lib/` -- `runtime.md`'s "Building dn-glibc" step 4. The runtime's
  glibc libraries are thus found before the tree's same-named `libc6` files,
  and the tree's copy is never loaded.
- `/proc/self/exe`: the loader records the real program path (`__dn_prog`,
  exported as `__dn_prog_get` in `elf/rtld.c`/`elf/Versions`), and
  `readlink()` of `/proc/self/exe` answers with it through
  `dn_policy_exe_link()`. Needed because a rule-3 launch runs through
  `RT/ld.so` (the exec gate), so the kernel's own `/proc/self/exe` is the
  loader, not the program; a plain `PT_INTERP` launch has no record and the
  kernel value is reverse-translated as before.
- `elf/rtld.c`: the loader maps the fixed gate page -- 4 KB at `P_GATE`
  (`0x100000000`, chosen by scanning real on-device process maps for an
  address free in every one, inside the 39-bit range) with
  `MAP_FIXED_NOREPLACE`, writes a bare `svc #0; ret` stub, and `mprotect`s
  it `r-x`. If the address is taken the mapping simply fails and every call
  falls back to a direct `svc` (the `ptrace` tier), the documented
  behavior. The loader exports the address as `__dn_gate_get()` for libc.
- `sysdeps/unix/sysv/linux/aarch64/sysdep.h` + `syscallS.S`: libc's
  `INTERNAL_SYSCALL_RAW` and the C `syscall()` entry now issue from the gate
  page when `__dn_gate` is set, else a direct `svc` (the loader build keeps
  the direct `svc`). `dn_policy_gate_probe()` (in the glue) copies
  `__dn_gate_get()`'s value into `__dn_gate` on the first path call. The
  cancelable path (`syscall_cancel.S` -- `open`/`read`/`write`) is
  deliberately left direct for now: its `_arch_start`/`_end` cancellation
  markers assume the `svc` is inline, and branching to the gate moves the PC
  out of range.
- `elf/Makefile`: `dn_policy_fake_stat` joins `rtld-stubbed-symbols` -- see
  below.

No `@DN_PREFIX@`: dn-policy derives the tree at runtime, so this patch is
prefix-agnostic (principle 5) like dn-policy itself, and needs no
substitution when applied.

## Why the `IS_IN (rtld)` guards and the stub are load-bearing

`elf/librtld.map` links the loader's minimal `dl-allobjs.os` against all of
`libc_pic.a` to discover which libc members the loader needs; the `malloc`
family is stubbed there so a stray reference cannot drag the real `malloc`
in, and any duplicate symbol is a hard error. Two consequences:

- Any glibc object the loader needs (it already needs `openat64.os`, and now
  also `fstatat64.os`/`faccessat.os`) must not, from the map's point of view,
  reach a dn-policy object that uses `malloc`/`flock`. The stat wrapper's
  `dn_policy_stat_post()`, the `chown`/`chmod` fakes and the hardlink
  wrappers reach `dn-policy-fakeroot.o`/`dn-policy-hardlink.o`, so
  `dn_policy_fake_stat`, `dn_policy_owner_merge`, `dn_policy_link` and
  `dn_policy_unlink` are added to `rtld-stubbed-symbols` in
  `elf/Makefile` -- the sanctioned mechanism for exactly this case ("symbol
  discovery is not compatible with the libc implementation"). The
  path-mapping objects (`dn-policy.o`, `dn-policy-glue.o`) need no stub:
  they use only leaf libc calls.
- Every call into dn-policy is `#if !IS_IN (rtld)`, so the rtld rebuilds
  that actually link into `ld.so` (`rtld-openat64.os`, `rtld-fstatat64.os`,
  ...) reference nothing dn-policy; rtld keeps the self-contained
  `__dn_redirect`. `dn_policy_stat_post()` (and `dn-policy-glue.c`'s use of
  it) is guarded the same way.

The lesson that shaped the leaf-call discipline: a path in the wiring that
reaches `snprintf` drags glibc's `vfprintf` machinery (`malloc`,
`__syscall_cancel`, `sbrk`, `__libc_fatal`) into the loader's object set,
where `dl-allobjs.os` already defines them -- which is what first broke the
build. Hence `dn-policy` uses only leaf libc calls (no `snprintf`/
`strtok_r`/`getenv`/...), as its own header comment explains.

## Applying it

```sh
# inside a work/ that already has dn-glibc-android.patch applied
patch -p1 < <repo>/patches/dn-policy-glibc-wiring.patch
```

Not part of the build workflow yet: `.github/workflows/build-glibc.yml`
applies only `dn-glibc-android.patch`. Applied by hand in the on-device
scratch build until P2 is complete and the tree can actually switch.

## Regenerating it

```sh
diff -ruN --exclude='.pc' --exclude='debian' work-after-official-patch work \
  > dn-policy-glibc-wiring.patch
```

Round-trip-verify before trusting it (fresh copy of
`work-after-official-patch`, apply, diff against `work` -- expect nothing):

```sh
cp -r work-after-official-patch /tmp/roundtrip
( cd /tmp/roundtrip && patch -p1 < dn-policy-glibc-wiring.patch )
diff -rq --exclude='.pc' --exclude='debian' /tmp/roundtrip work
```

## Status

Build-verified and runtime-verified (2026-10-08) against glibc
`2.41-12+deb13u4`. `make -O -j8` with this patch applied links cleanly
(exit 0, no `multiple definition`); `ld.so` ends up with no dn-policy symbol
(the loader stays on `__dn_redirect`), `libc.so` carries the full dn-policy.

A minimal prefix -- the built `ld.so`/`libc.so.6` laid out under a tree with
an `etc/dn-runtime-marker` and an absolute in-tree symlink -- was run directly
under the built loader. `open`, `stat` and `lstat` of `/etc/...` all
translate into the tree; `lstat` honors `AT_SYMLINK_NOFOLLOW`; `stat` follows
the symlink; a missing path still gives `ENOENT`; and the fake-root
post-processing really runs (`dn-policy-fakeroot.o`/`-hardlink.o` are
reached -- `RT/state/owners.db` and `state/links/` appear once a stat goes
through). The same binary run under the host loader fails, so the translation
is what made it work.

A second binary, exec'd with the prefix's own loader as its `PT_INTERP` (the
real launch shape) and placed inside the tree, confirms the reverse
translation: `getcwd()` inside the tree returns `/etc`; `readlink()` of
`/proc/self/cwd`, `/proc/self/exe` and `/proc/self/fd/N` returns `/etc`,
`/usr/bin/dn-rt-rev` and `/etc/dn-runtime-marker`; and an ordinary in-tree
symlink's stored target comes back unchanged.

A third binary does the manipulation round trip: `mkdir`, `truncate`,
`symlink`+`readlink`, `rename`, `utimes` and `statfs` all succeed, and the
shell then finds the result exactly where it belongs --
`TREE/etc/dn-path-work/data` (4 bytes, mtime 2001) and `sym2` as a symlink
-- with no host `/etc/dn-path-work` created.

A fourth binary calls the kernel with `syscall(2)` directly:
`syscall(SYS_openat/newfstatat/faccessat/readlinkat ...)` on `/etc/...` all
reach the tree (right content, `AT_SYMLINK_NOFOLLOW` honored, `ENOENT`
preserved), `syscall(SYS_getpid)` still works, and `syscall(SYS_mkdirat,
...)` creates `TREE/etc/dn-sc-work` with no host `/etc/dn-sc-work`.

A fifth binary checks the gate page: `/proc/self/maps` shows it mapped `r-xp`
at `100000000` (the chosen `P_GATE`), and `__dn_gate_get()` returns
`0x100000000`. libc's `INTERNAL_SYSCALL_RAW` and `syscall()` entry now issue
from it (the non-cancelable path), which every other check exercises -- a
wrong macro would break every syscall.

A sixth binary covers fake root's writes: `chown("/etc/...", 1234, 5678)`
then `stat()` shows `1234:5678`; `chmod("/etc/...", 04755)` then `stat()`
shows mode `04755` (real bits `0755`, setuid recorded) with the owner still
`1234:5678` -- the merge holds both.

A seventh binary covers hardlinks: `link()` makes `/etc/dn-link-b` for
`/etc/dn-link-a`, both report as regular files with the same inode and
`st_nlink == 2` (on disk both are symlinks into `RT/state/links/`), reading
either works, and `unlink()` on one leaves the other with `st_nlink == 1`.

An eighth check covers the library search order: with the same
`libdnorder.so` (one SONAME) placed in both `RT/lib` and the tree's
`/usr/lib`, a program with no rpath loads the `RT/lib` one (`dn_order == 1`);
remove that copy and it loads the tree's (`dn_order == 2`).

A ninth check runs the reverse-translation binary in the rule-3 shape
(`ld.so --argv0 <name> <real path> ...`): `readlink("/proc/self/exe")` then
reports the real program path, not the loader; the plain `PT_INTERP` launch
reports it too.

Covers the public `open`/`openat` and `stat`/`fstatat`/`statx`/`faccessat`
families, reverse translation (`getcwd`, the `/proc/self` magic links, and
`/proc/self/exe` answering with the real program path under a rule-3 launch),
the `mkdir`/`rmdir`/`rename{,at,at2}`/`symlink`/`truncate`/`utimensat`/`statfs`
wrappers, the `syscall(2)` interposition for the path group, fake root's
writes (`chown`/`lchown`/`chmod`/`fchmodat`), hardlinks (`link`/`unlink`),
the loader's gate-page mapping with libc issuing its syscalls from it (the
non-cancelable path), and the `RT/lib`-first library search order. Still
open: the xattr family and the public `fchownat()`
(`syscalls.list`-generated), `chdir`/`chroot`, the cancelable
`syscall_cancel.S` path, and the seccomp filter's gate-IP rule in `dn-trace`.

**Known issue (accepted): the gate page is used, but the filter has no
gate-IP rule yet -- on purpose.** Turning that rule on (ALLOW a path/identity
call whose instruction pointer is in the gate page) requires that *every*
path-group and identity-group call dn-glibc issues from the gate is already
translated or rewritten in-process -- otherwise the filter would let an
untranslated call through unchecked. Two gaps remain:

- `bind` (a UNIX socket path lives in the sockaddr) is a C wrapper using the
  syscall macro, so it would issue from the gate untranslated;
- the whole identity group (`getuid`/.../`setuid`/`fstat`/`fchown`/
  `fchownat`) is not wired in dn-glibc yet, so gate-issued ones would skip
  both dn-glibc's fake root and `dn-trace`'s.

The `syscalls.list`-generated wrappers are *not* a gap: they issue a raw
`svc` from `syscall-template.S` (asm `DO_CALL`), not through the macro, so
they never come from the gate -- they are traced and handled by ptrace, as
the tier model intends. Until the two gaps are closed the gate-IP rule stays
off and every group call is traced (correct, just slower). This is the
intended transitional state, not a regression.
