# What the libc shim must intercept

Which libc entry points `native/path-redirect.c` has to cover for the packages
we support, and the ones it does not. The scope is [`standard.md`](standard.md);
the design is [`design.md`](design.md).

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

1. **Scope the corpus** — `scripts/scope-sample.py` reads a Debian
   `binary-arm64/Packages` index and selects the packages whose `Section` is
   in `standard.md`'s user scope. This is the whole point of choosing a scope:
   it drops the archive from ~64 000 binary packages to the sample below.
2. **Collect imported symbols** — `scripts/scan-libc-symbols.sh` walks every
   ELF in the corpus, reads the undefined entries of `.dynsym`
   (`readelf --dyn-syms`), and counts them; it marks an ELF with no dynamic
   section as `STATIC_ELF`.
3. **Intersect** with the universe above, and check which names the shim
   already defines.

Reproduce (one command at a time, on the phone):

```
mkdir -p ~/debcorpus/debs && cd ~/debcorpus
curl -s -o Packages.gz https://deb.debian.org/debian/dists/stable/main/binary-arm64/Packages.gz
python3 ~/deb-native/scripts/scope-sample.py Packages.gz > sel.tsv 2> scope.txt
cut -f2 sel.tsv | while read u; do curl -s -o debs/$(basename "$u") "$u"; done
sh ~/deb-native/scripts/scan-libc-symbols.sh --debs debs > scan-apps.txt
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

### Gaps found

**Genuine, and cheap to close** (the symbol exists in Termux's glibc, the shim
just does not define it — all take a plain path or template):

| symbol | app/base | note |
|---|---|---|
| `__xstat` / `__lxstat` / `__fxstat` | 1 / 0 each | legacy pre-2.33 stat entry points; Termux's glibc still exports them, so an old binary reaches the real `/etc` |
| `sendmsg` | 2 / 1 | AF_UNIX datagram path in `msghdr.msg_name`; mirror of the existing `sendto` |
| `lutimes` | 2 / 0 | `utimes` for the symlink itself |
| `mkstemps` / `mkostemps` | 0 / 1 | template form of the already-covered `mkstemp` |
| `eaccess` | 1 / 0 | glibc alias of `euidaccess` |
| `setmntent` | 1 / 0 | opens an mtab/mounts file by path |

**Covered indirectly — verify, do not assume a gap:** `scandir64` (1/1) and
`glob`/`glob64` (1/0) take a directory path but walk it through the already
interposed `opendir`/`stat`, so the redirect should already happen one level
down.

**NSS lookups — needs a test, not yet a confirmed gap:** `getpwuid` (31/3),
`getgrgid` (17/1), `getpwnam` (3/3), `getaddrinfo` (8/0), `gethostbyname`
(3/0), `getservbyname` (1/0). These take no path; they read `/etc/passwd`,
`/etc/group`, `/etc/hosts`, `/etc/resolv.conf`. glibc's `nss_files` backend is
a separate object that opens those files through the interposable `fopen`, so
they may already be redirected — but that must be shown with a fake prefix and
a modified `/etc/passwd`, not inferred.

**Out of scope, deliberately:** `mount` (0/1), `umount2` (0/1), `chroot`
(1/1) are admin operations; redirecting them is neither possible nor wanted.

### The real boundary

**Raw `syscall()`: 12 in-scope ELFs import it, 0 in the base set.** These
programs name syscalls directly and bypass the shim entirely. **Static
binaries: 0** in the sample, so historically rarer than the review feared —
but the category is real. Both need a syscall-level mechanism
(`ptrace`/`SECCOMP_RET_USER_NOTIF`, feasible here — `proot` runs). That work
is tracked in [#1](https://github.com/jronminh/deb-native/issues/1) and
`TODO.md`, and is distinct from the shim: **the shim is now as complete as the
libc layer can be.**

## Next steps

- [ ] Add the cheap gaps above (`__xstat`/`__lxstat`/`__fxstat` + `*64`,
      `sendmsg`, `lutimes`, `mkstemps`/`mkostemps`, `eaccess`, `setmntent`).
- [ ] Test the NSS question against a fake prefix (`/etc/passwd` under
      `$INSTDIR`) and record the answer here.
- [ ] Extend `scan-libc-symbols.sh` to print the *file* behind `syscall()` and
      `STATIC_ELF`, so the tracer's first targets are named.
- [ ] Re-run on a wider/`sid` sample and on the in-scope set of `sudo-less`'s
      survey list once the classifier exists.
