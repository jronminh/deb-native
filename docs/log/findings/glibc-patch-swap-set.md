# Findings: the exact swap set for Debian `libc6`/`libc-bin` (2026-10-03)

<!-- template: templates/docs.template.md -->

**Impact: Isolated.** An alternative deployment for the `dn-glibc` prefix is
to install Debian's own `libc6`/`libc-bin` `.deb`s unchanged and swap in only
the files our Android patch actually changes, instead of shipping a full
rebuilt glibc. This entry pins that swap set exactly: **10 files**, found
from the glibc source plus the debug info in our own build. Most of the
patch's effect is already inside `libc.so.6`; the rest of the 283 files that
differ from Debian differ only because of build config/toolchain, not the
patch. No code changed -- this is the analysis a deployment plan needs.

## Contents

- [Context](#context)
- [Why a binary comparison fails](#why-a-binary-comparison-fails)
- [Method](#method)
- [The swap set](#the-swap-set)
- [Not swapped](#not-swapped)
- [NSS stubs are upstream design](#nss-stubs-are-upstream-design)
- [Limits](#limits)
- [Reproduce](#reproduce)

## Related docs

- [`../../spec/dn-glibc-prefix.md`](../../spec/dn-glibc-prefix.md) -- the
  next-gen prefix this swap would deploy.
- [`../../spec/shim/shim-coverage.md`](../../spec/shim/shim-coverage.md) -- documents
  the NSS stubs and why NSS reads are not shimmable.
- [`own-glibc-missing-libc-bin.md`](own-glibc-missing-libc-bin.md) -- why
  `libc-bin` is path-sensitive and must be this project's build.
- [`../../../third_party/glibc-android-patches/README.md`](../../../third_party/glibc-android-patches/README.md)
  -- the patch and how it is applied with a `@TERMUX_PREFIX@` substitution.

## Context

Deploying `dn-glibc` could install Debian's real `libc6` and `libc-bin`
packages as-is (real, held packages) and then overwrite only the handful of
files the Android patch changes. That avoids maintaining a full forked glibc
`.deb`, and shrinks the per-prefix rebuild to those few files. Its
correctness depends entirely on knowing the swap set exactly.

## Why a binary comparison fails

Comparing our rebuilt files against Debian's real package (after stripping
debug, `.comment` and `.note.gnu.build-id` so only code/data is compared)
still flags **283 files**: every `gconv` module, `libm`, `libanl`, `libdl`,
and more -- none of which the patch touches. The cause is build config, not
the patch: our build uses `--disable-multi-arch` and a different toolchain,
so *every* object differs from Debian's. "Differs from Debian" is therefore
not the same as "patched", and cannot be used to derive the set.

## Method

The set is derived from the **source**, not the binaries, in two layers:

1. **Direct code.** Files whose `.c`/`.S` the patch edits. A changed `.c`
   always changes its object.
2. **Header-induced.** The patch also edits **24 headers**. Only files whose
   compiled code *uses* a changed macro/identifier change; a changed header
   alone does not. For example `posix/unistd.h` only gains a declaration
   (`syscallS`) -- a no-op for binaries -- and `libio/stdio.h` only changes
   the `P_tmpdir` macro, which few callers use.

Each affected source is then mapped to the installed output using the
**DWARF line-table of our own build**: join
`include_directories[dir_index] + name` and normalise to a repo-relative
path, then intersect with the patch's changed paths. This resolves same-name
headers (`stddef`, `unistd.h`, `netdb.h`, `paths.h`) that a basename match
would confuse.

Counts: **103** changed code sources, **24** changed headers.

## The swap set

The swap is these **10 files** (paths relative to the prefix; all under
`usr/lib/aarch64-linux-gnu/` for the libraries):

| # | file | why |
| --- | --- | --- |
| 1 | `usr/lib/aarch64-linux-gnu/libc.so.6` | 55 patched sources (nss, sysvipc, syscall wrappers, `android_passwd_group.c`, ...) plus the header-induced callers (`res_init.c`, `nss_database.c`, `tmpnam.c`, ...) |
| 2 | `usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1` | `elf/rtld.c` (drops inherited `LD_PRELOAD`, prefixed `ld.so.preload`), `dl-execstack.c`, `mprotect.c`, ... |
| 3 | `usr/lib/aarch64-linux-gnu/libresolv.so.2` | `resolv/compat-gethnamaddr.c` uses the patched `_PATH_HOSTS`/`_PATH_*` macros |
| 4 | `usr/lib/aarch64-linux-gnu/libnsl.so.1` | `nis/nis_call.c`, `nis/nis_file.c`, `nis/ypclnt.c` |
| 5 | `usr/lib/aarch64-linux-gnu/libnss_compat.so.2` | `nss/nss_compat/compat-*.c` |
| 6 | `usr/lib/aarch64-linux-gnu/libnss_hesiod.so.2` | `hesiod/hesiod.c`, `resolv/resolv.h` |
| 7 | `usr/lib/aarch64-linux-gnu/librt.so.1` | `sysdeps/generic/unwind-resume.c` |
| 8 | `usr/sbin/ldconfig` | `elf/ldconfig.c` plus 18 libc objects linked statically into it (it is our 4.76 MB build) |
| 9 | `usr/bin/localedef` | `locale/programs/localedef.c`, `linereader.c` |
| 10 | `usr/bin/iconv` | links `locale/programs/linereader.c` |

## Not swapped

Everything else in `libc6`/`libc-bin` can be taken from Debian's package
unchanged. Confirmed unaffected, despite differing byte-for-byte from
Debian: all `gconv/*`, `libm.so.6`, `libanl.so.1`, `libdl.so.2`,
`libpthread.so.0`, `libutil.so.1`, `libBrokenLocale.so.1`, `libmvec.so.1`,
`libc_malloc_debug.so.0`, `libmemusage.so`, `libpcprofile.so`,
`libthread_db.so.1`, `libnss_files.so.2`, `libnss_dns.so.2`, and the
`libc-bin` programs `getconf`, `getent`, `locale`, `pldd`, `zdump`,
`tzselect`, `zic`, `iconvconfig`. Their header matches were all the
declaration-only `unistd.h` or the unused `P_tmpdir`/`_PATH_*` macros.

## NSS stubs are upstream design

`libnss_files.so.2` and `libnss_dns.so.2` contain no NSS code in our build --
but this is **expected, not a build bug**: `libc.so.6` itself defines
`_nss_files_*`/`_nss_dns_*`, and the two modules are empty ABI stubs. Stock
Debian glibc is identical. The full account is in
[`../../spec/shim/shim-coverage.md`](../../spec/shim/shim-coverage.md) ("NSS lookups --
confirmed out of the shim's reach"); the patch's `nss/nss_files/files-*.c`
edits therefore land in `libc.so.6`, which is already file 1 above. The two
stubs are correctly *not* in the swap set.

## Limits

- This covers `libc6` and `libc-bin` only. If `libc6-dev`/`libc-dev-bin`
  are shipped, their static libraries and dev programs also carry patched
  code and would need the same treatment.
- The header layer is classified by whether compiled code uses the changed
  identifier; unusual cases (a macro used only in inline functions, or an
  asm-level change) would still need a patched-vs-unpatched build to settle.
  A pristine build with the same configure is the definitive check if doubt
  remains.
- Any swap must be built for the **target prefix**: paths such as the
  loader's cache file, `ldconfig`'s sysconfdir and the `paths.h` macros are
  baked at compile time, so a `.dn` build cannot be dropped into another
  prefix. See `own-glibc-missing-libc-bin.md`.

## Reproduce

On `fe2`, extract `~/dn-glibc-build/dist-selfderive/libc6.deb` and
`libc-bin.deb`, then for each ELF dump its DWARF line-table
(`llvm-dwarfdump --debug-line`), join `include_directories[dir_index] + name`
into a repo-relative path, and intersect with the patch's changed paths
(`grep -o '^+++ work/[^[:space:]]*' dn-glibc-android.patch`). Two layers:
changed `.c/.S` gives the baseline; changed headers, filtered to files whose
code actually uses the changed identifier, add `libresolv.so.2`.
