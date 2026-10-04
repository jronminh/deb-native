# What the libc shim must intercept

<!-- template: templates/docs.template.md -->

Which libc entry points `native/path-redirect.c` has to cover for the packages
we support, and the ones it does not. The scope is [`standard.md`](../../reference/standard.md);
the design is [`path-shim.md`](path-shim.md).

## Contents

- [First, the layer: libc functions, not syscalls](#first-the-layer-libc-functions-not-syscalls)
- [The universe](#the-universe)
- [Method](#method)
- [Results](#results)
- [Resolved (2026-10-01): `/lib`, `/bin`, `/sbin` added; `/run`, `/lib64` still open](#resolved-2026-10-01-lib-bin-sbin-added-run-lib64-still-open)
- [Next steps](#next-steps)

## Related docs

- [`standard.md`](../../reference/standard.md) — the scope this doc's corpus is drawn
  from.
- [`path-shim.md`](path-shim.md) — the shim this doc measures coverage
  for.
- [`syscall-boundary.md`](../../reference/syscall-boundary.md) — what's left once this
  doc's coverage is accounted for.
- [`android-platform.md`](../../reference/android-platform.md) — the per-file fork
  verdict for the glibc patch that supersedes part of this shim's job.

## First, the layer: libc functions, not syscalls

A dynamically-linked program does **not** call the kernel directly. It calls a
libc function (`open`), and glibc issues the syscall (`openat`). The shim is an
`LD_PRELOAD` **symbol interposer**: it replaces the *libc function* in the
dynamic symbol table and rewrites the path before calling the real one.

So "does the shim handle all the dynamic syscalls" is a category slip:

- **syscalls** are the kernel ABI (`openat`, `statx`, `execve`, …). Every
  program makes them, static or dynamic; there is no per-program "dynamic
  syscall".
- **dynamic libc calls** are the imported symbols (`open`, `stat`, `execvp`,
  `dlopen`, …). The shim sits here.

Two things fall outside this layer and are *not* shim bugs:

1. a raw `syscall(SYS_openat, …)` call — it names the syscall directly and
   never touches the imported `open`;
2. a statically-linked binary, which imports no libc symbols at all.

Both are the syscall tracer's job (see [the real boundary](#the-real-boundary)).

## The universe

Every glibc entry point that takes a filesystem path is a candidate. Grouped:
`open`/`openat`/`creat`/`fopen`/`freopen`/`opendir` and their `*64` and
fortified `__*_2` names; `stat`/`lstat`/`fstatat`/`statx` and the legacy
`__xstat` family; `statfs`/`statvfs`; `access`/`faccessat`; `mkdir`/`unlink`/
`rename`/`link`/`symlink`/`readlink`/`chmod`/`chown`/`truncate` and the `*at`
forms; `utime`/`utimes`/`utimensat`/`lutimes`; `mkfifo`/`mknod`; the xattr
family; `realpath`/`canonicalize_file_name`; `exec*`/`posix_spawn`/`fexecve`;
`dlopen`/`dlmopen`; AF_UNIX `bind`/`connect`/`sendto`/`sendmsg`;
`inotify_add_watch`; the temp templates `mkstemp`/`mkostemp`/`mkstemps`/
`mkdtemp`; the NSS lookups (`getpwuid`, `getaddrinfo`, …) whose *files* live
under `/etc`; and the admin operations (`mount`, `chroot`, …).

## Method

1. **Scope the corpus** — `scripts/survey/scope-sample.py` reads a Debian
   `binary-arm64/Packages` index and selects the packages whose `Section` is
   in `standard.md`'s user scope. This is the whole point of choosing a scope:
   it drops the archive from ~64 000 binary packages to the sample below.
2. **Collect imported symbols** — `scripts/bench/scan-libc-symbols.sh` walks every
   ELF in the corpus, reads the undefined entries of `.dynsym`
   (`readelf --dyn-syms`), and counts them; it marks an ELF with no dynamic
   section as `STATIC_ELF`.
3. **Intersect** with the universe above, and check which names the shim
   already defines.

Reproduce (one command at a time, on the phone):

```
mkdir -p ~/debcorpus/debs && cd ~/debcorpus
curl -s -o Packages.gz https://deb.debian.org/debian/dists/stable/main/binary-arm64/Packages.gz
python3 ~/deb-native/scripts/survey/scope-sample.py Packages.gz > sel.tsv 2> scope.txt
cut -f2 sel.tsv | while read u; do curl -s -o debs/$(basename "$u") "$u"; done
sh ~/deb-native/scripts/bench/scan-libc-symbols.sh --debs debs > scan-apps.txt
```

### Corpus measured

- **base:** the 32 packages installed in `~/dn6` (the base bootstrap).
- **in-scope sample:** 43 user sections, 258 packages, 68 MB, from
  `stable/main` `binary-arm64`. 14 083 distinct undefined dynamic symbols.
- **static binaries:** **0** in the sample. `syscall()` importers: **12**
  (all in the sample; 0 in the base set).

## Results

Every high-frequency path-taking symbol a supported package imports is already
intercepted. The counts are `app / base` imports across the corpus:

| symbol | app/base | shim |
|---|---|---|
| `fopen` | 89 / 12 | yes |
| `remove` | 63 / 1 | yes |
| `mkdir` | 51 / 1 | yes |
| `open64` | 46 / 2 | yes |
| `unlink` | 38 / 8 | yes |
| `open` | 34 / 14 | yes |
| `__open64_2` (fortified) | 34 / 1 | yes |
| `access` | 30 / 9 | yes |
| `stat` | 28 / 12 | yes |
| `opendir` | 28 / 8 | yes |
| `rename` | 25 / 8 | yes |
| `stat64` | 24 / 3 | yes |
| `chdir` | 21 / 3 | yes |
| `link` | 21 / 2 | yes |
| `lstat64` | 16 / 3 | yes |
| `freopen` | 16 / 0 | yes |
| `execvp` | 15 / 2 | yes |
| `chmod` | 14 / 4 | yes |
| `readlink` | 13 / 7 | yes |
| `chown` | 10 / 4 | yes |
| `bind`/`connect` | 10 / 2, 9 / 3 | yes |
| `dlopen` | 8 / 5 | yes |
| `sendto` | 6 / 0 | yes |
| `realpath` | 6 / 1 | yes |
| `mkstemp` | 5 / 0 | yes |
| `posix_spawn*` | 1 / 0 | yes |

### Gaps found — and closed

Nine genuine gaps were all closed in `native/path-redirect.c`:
`__xstat`/`__lxstat` (legacy pre-2.33 stat entry points; `__fxstat` is
fd-based and needs no redirect), `__fxstatat64` (the 64-bit `fstatat`, which
Bun/Node call directly -- without it the real owner leaked past fake-root and
Claude Code refused its own temp dir), `sendmsg` (the AF_UNIX path in
`msghdr.msg_name`, mirroring `sendto`), `lutimes`, `mkstemps`/`mkostemps`,
`eaccess`/`euidaccess`, and `setmntent`. `scandir`/`scandir64` were promoted
from "indirect" to explicit redirects as well: glibc's own scan walks the
directory with an internal `opendir` that does not reach the interposed
symbol. `tests/shim-libc/run.sh` exercises every one of them on-device.

**Still indirect (not a gap):** `glob`/`glob64` (1/0) match a pattern and
walk it through the interposed `opendir`/`stat`, so the redirect happens one
level down; a rewritten *pattern* would, however, return `$INSTDIR`-prefixed
matches, so it is left alone.

**NSS lookups — confirmed out of the shim's reach.** `getpwuid` (31/3),
`getgrgid` (17/1), `getpwnam` (3/3), `getaddrinfo` (8/0), `gethostbyname`
(3/0), `getservbyname` (1/0) take no path; they read `/etc/passwd`,
`/etc/group`, `/etc/hosts`, `/etc/resolv.conf`. The shim cannot rewrite the
call, so the question was whether glibc's backend opens those files through
the interposable `fopen` anyway. The test says **no**: with a fake
`$INSTDIR/etc/passwd` holding `dnshim:54321`, `getpwnam("dnshim")` and
`getpwuid(54321)` returned `NOTFOUND`, and `getgrgid(54321)` returned the
**real** Android group `all_a4321`. Running the same test with
`DN_REDIRECT_DEBUG=1` produces **no** rewrite line for `nsswitch.conf`,
`passwd`, `group`, `hosts` or `resolv.conf` — every one of those opens is
internal.

Why — and why it is not a Termux packaging bug: `libc.so.6` itself defines
`_nss_files_*` and `_nss_dns_*`, and the bundled `libnss_files.so.2` /
`libnss_dns.so.2` are empty ABI stubs (zero `_nss_*` symbols, zero imports),
so glibc never `dlopen`s them. Stock Debian glibc is the same — its
`libc.so.6` also defines `_nss_files_getpwnam` and its `libnss_files.so.2`
is a stub — so this is **upstream glibc design**. The opens go through the
private `__open_nocancel`/`__open64_nocancel` (`GLIBC_PRIVATE`), bound at
link time, which no `LD_PRELOAD` interposer can reach. That also rules out
the "ship a custom NSS module" route: the dispatch config (`nsswitch.conf`)
is read internally too, so it cannot be pointed at a module of ours.

**Not unfixable — just not at this layer.** The syscall tracer sees the
`openat` syscall before any of this and covers NSS, raw `syscall()` and
static binaries uniformly. A narrower libc-layer hack exists — interpose the
public `getpwnam`/`getpwuid`/`getpwuid_r`/… and reimplement only the `files`
lookup against `$INSTDIR/etc/passwd` — but it is a partial reimplementation
(no `hosts`/`dns`, no `nsswitch` semantics, `_r` variants) and the tracer is
the cleaner general fix. In practice most lookups just resolve the current
uid/gid or hostnames, where the real `/etc` (and Android's DNS) is often
what you want anyway.

**Resolved 2026-09-26:** the tracer route is wired in `native/dn-run.c` and
the actual fix is a bind of the prefix's `/etc` over Termux glibc's
**sysconfdir** `$PREFIX/glibc/etc` (where NSS reads, not the guest `/etc`).
See `syscall-boundary.md`, "Solved: NSS", and `tests/tracer-nss/run.sh`.

**Out of scope, deliberately:** `mount` (0/1), `umount2` (0/1), `chroot`
(1/1) are admin operations; redirecting them is neither possible nor wanted.

### Implementation notes (from closing the gaps, 2026-09-26)

Two of the added symbols needed more than a rewrite-and-call:

- **`mkstemp`/`mkostemp`/`mkdtemp`/`mkstemps`/`mkostemps` modify the
  caller's template in place**, and that buffer is only
  `strlen(template)+1` long — rewriting it to `$INSTDIR/etc/...` cannot be
  copied back as-is. The shim calls the real function on the rewritten
  buffer, then copies back only the part after `$INSTDIR` (the random
  suffix included), after a length check against the caller's original
  template. Verified: the caller sees `/etc/zz_mkstemp9wnoFI` while the
  file is created under `$INSTDIR/etc/`.
- **`posix_spawn` bypasses the interposed `execve`** (glibc uses
  `clone`+`exec` internally), so it gets its own wrapper: rewrite the path,
  keep the environment for a glibc target and swap in `bionic_env()`
  otherwise. `posix_spawnp` walks `$PATH` itself (glibc's internal walk is
  invisible here) and delegates to `posix_spawn`. This matters because
  modern glibc and coreutils spawn helpers through `posix_spawn`, not
  `fork`+`execve`.
- `sendmsg` rewrites the AF_UNIX address in a copied `msghdr` the same way
  `sendto` does.

On-device verification (`tests/shim-libc/run.sh`): builds a standalone
glibc test binary, sets up a fake `$DN_INSTDIR` root, runs it under the
shim with `DN_REDIRECT_DEBUG=1`, and asserts every intercepted symbol
rewrote its path (46 assertions) with nothing leaking into the real
`/etc`. Two on-device facts made the test binary buildable at all: the
glibc side-install does ship `Scrt1.o`/`crti.o`/`crtn.o` (an earlier note
assumed a standalone glibc executable could not be linked, but that was
only because it looked for `crtbeginS.o`/`crtendS.o`/`libgcc.a`; linking
with `-nostartfiles -nodefaultlibs` and naming those three objects
explicitly works with the same clang that builds the shim); and the
`posix_spawn` test copies a glibc binary under `$INSTDIR/usr/bin/` and
spawns it there, proving the redirect reached the real spawn, not just the
test process's own libc calls.

### The real boundary

**Raw `syscall()`: 12 in-scope ELFs import it, 0 in the base set.** These
programs name syscalls directly and bypass the shim entirely. **Static
binaries: 0** in the sample, so historically rarer than the review feared —
but the category is real. The same ceiling applies to libc-internal file
access: the NSS reads above happen inside `libc.so.6` and never cross the
interposable symbol. All three need a syscall-level mechanism
(`ptrace`/`SECCOMP_RET_USER_NOTIF`, feasible here — `proot` runs). That work
is tracked in [#1](https://github.com/jronminh/deb-native/issues/1) and
`TODO.md`, and is distinct from the shim: **the shim is now as complete as the
libc layer can be.** The wider boundary — inline `svc #0`, static executables,
explicit `syscall()`, and the `PT_INTERP` routing gap in `dn-run.c` — is mapped
and measured in [`syscall-boundary.md`](../../reference/syscall-boundary.md).

## Resolved (2026-10-01): `/lib`, `/bin`, `/sbin` added; `/run`, `/lib64` still open

The five-prefix view (`/usr`, `/etc`, `/var`, `/opt`, `/root`) turned out too
narrow in practice, not just by inspection: `apt install gcc` end to end
(`docs/log/findings/gcc-hello-pt-interp-gap.md`) hit it directly — `ld` failed with
`cannot find /lib/aarch64-linux-gnu/libc.so.6` because `gcc`'s own linker
invocation hardcodes `-dynamic-linker /lib/ld-linux-aarch64.so.1` and searches
`/lib/aarch64-linux-gnu` regardless of how the package that shipped it spells
paths internally. That is exactly the "program hardcodes the literal `/bin/x`
or `/lib/x.so`" gap predicted below, just surfaced by the compiler toolchain
instead of found by corpus inspection first.

Fix: `path-redirect.c`'s `rewrite()` now also dispatches `/lib` and `/bin`
(second byte `'l'`/`'b'`, default `prelen = 4`) and `/sbin` (second byte
`'s'`, `prelen = 5` like `/root`) — none collide with the existing five.
These three are Debian's merged-usr symlinks into `/usr/{lib,bin,sbin}`
anyway (the prefix's own `base-files` sets them up the same way, confirmed:
`$DN/bin -> usr/bin`, `$DN/lib -> usr/lib`, `$DN/sbin -> usr/sbin`), so
redirecting the literal prefix and then following the real symlink lands in
the same place `/usr/...` already did — this completes that existing
coverage rather than adding a new one. `/bin/sh` etc.'s execve-specific
carve-out (`path-redirect.c:307-313`, line numbers now shifted by this
addition) is unaffected and still separately necessary (execve, not
open/stat).

Still open, deliberately not added yet (no measured need so far):

- **`/lib64`** — not used on Debian arm64 (that's an x86_64 convention); no
  evidence it's needed here.
- **`/run`** — modern packages (systemd-era sockets, PID files) commonly use
  `/run/...` directly rather than `/var/run/...`. Add if and when something
  actually hits it, same as `/lib` above — don't add blind.

## Next steps

- [x] Add the cheap gaps above (`__xstat`/`__lxstat` + `*64`, `sendmsg`,
      `lutimes`, `mkstemps`/`mkostemps`, `eaccess`, `setmntent`, and
      `scandir`/`scandir64`); all now intercepted and in `tests/shim-libc`.
- [x] Test the NSS question against a fake prefix — **not redirected**: the
      `files` service is inside `libc.so.6` and opens via private
      `__open*_nocancel` (see above); the syscall tracer is the only
      mechanism.
- [ ] Extend `scan-libc-symbols.sh` to print the *file* behind `syscall()` and
      `STATIC_ELF`, so the tracer's first targets are named.
- [ ] Re-run on a wider/`sid` sample and on the in-scope set of `sudo-less`'s
      survey list once the classifier exists.
