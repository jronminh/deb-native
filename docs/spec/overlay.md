# The overlay: dn-policy, dn-glibc, dn-trace

<!-- template: templates/docs.template.md -->

How a prefix runs stock Debian `.deb` packages intact, once installed: every
path-touching syscall resolves through one shared policy, dynamic glibc
binaries take a native-speed fast path, everything else (static, Go, foreign
loaders such as AppImage/conda/Electron/musl) falls back to a traced path with
identical results. Runtime speed is the priority; install time may be slow.
This is the **overlay**: the runtime pieces that sit in the tree's `RT`
directory and are built once per (CPU architecture, `libc6` ABI version). It
replaces the whole older overlay — an `LD_PRELOAD` path shim, an
adopt-on-first-run launcher, a run-time `PT_INTERP` rewriter, and a per-script
maintainer-script `sed` rewrite — rather than running alongside it.

Status: the fast-path wiring (dn-glibc) and the tracer (dn-trace) exist and
are exercised on-device; see [Status](#status).

## Contents

- [Goal and principles](#goal-and-principles)
- [What this replaces](#what-this-replaces)
- [Components and boot flow](#components-and-boot-flow)
- [The exec gate](#the-exec-gate)
- [The seccomp filter and its tiers](#the-seccomp-filter-and-its-tiers)
- [dn-policy](#dn-policy)
- [dn-glibc](#dn-glibc)
- [The syscall catalog](#the-syscall-catalog)
- [apt and dpkg under the runtime](#apt-and-dpkg-under-the-runtime)
- [Init, services, and hard limits](#init-services-and-hard-limits)
- [Status](#status)

## Related docs

- [`prefix.md`](prefix.md) — the tree and the tarball: how the overlay is
  assembled into a prefix and shipped.
- [`../../src/dn-policy/`](../../src/dn-policy) — the policy library itself.
- [`../../src/tracer/`](../../src/tracer) — `dn-trace`, the tracer.
- [`../../src/syscalls.tsv`](../../src/syscalls.tsv) — the syscall catalog.
- [`../../patches/README.md`](../../patches/README.md) — the two glibc patches.

## Goal and principles

Goal: install stock Debian `.deb` packages into a tree without modifying them.
A dynamic glibc program runs at native speed; everything else falls back to a
traced path with the same result. Runtime speed is the priority; install may
be slower.

Five principles, everything below follows them:

1. **No per-package patching.** Every adjustment lives in the runtime and
   applies by syscall class, not by package.
2. **The unit of decision is the individual syscall.** Classifying a program
   only picks the initial filter.
3. **One policy, enforced in two places.** The rules for path rewriting, fake
   root, and hardlinks live in one shared library (dn-policy). The glibc
   runtime (fast path) and the tracer (fallback path) both call it, so the two
   paths can never disagree.
4. **Push each call to the cheapest tier that can handle it.** Optimize the
   distribution of calls across tiers, not each tier in isolation.
5. **dn-policy hardcodes nothing prefix-specific.** Every difference between
   prefixes (core-ultra, core-deb, specialized) is data read at run time, not
   something rebuilt. `TREE`/`RT` self-derive from the loader's own path (as
   glibc's prefix self-derivation already does); the path-exception list and
   the owner-store backend choice live in a config file or are probed at
   startup; `P_GATE` is a device/kernel property (probed once, stored), not a
   prefix property. Consequence: dn-policy, dn-glibc, dn-trace and the seccomp
   filter need only one build per (CPU architecture, `libc6` ABI version),
   reused unmodified across every tarball on that version — never rebuilt per
   prefix. The one thing legitimately hardcoded is the architecture's
   syscall-number list (see [the catalog](#the-syscall-catalog)), since it is
   an ABI property identical across every prefix on one architecture.

## What this replaces

| Prior approach | This design | Why |
| --- | --- | --- |
| Patch `PT_INTERP` and rpath per package | Dropped. The exec gate switches the loader at exec time | No per-package patching; foreign programs are covered too |
| `LD_PRELOAD` path-redirect shim | Dropped. Path rewriting lives in dn-glibc | Survives a program clearing its environment; reaches glibc's own internal calls and `dlopen` |
| First-run trial ("adopt everything") to classify a program | Dropped. Classification is static, at exec time | One run never proves later runs; the filter catches a mismatch the moment it happens |
| A per-process filter keyed to glibc's address | Dropped. One shared filter, installed once at the root, recognizes dn-glibc via a fixed gate page | The filter survives `exec`; one per process stacks up and misapplies addresses to children |
| Trap `SIGSYS` inside the process | Dropped | A static program exec'd afterward would receive `SIGSYS` and die |
| A hand-written resolver in place of `apt` | Kept temporarily through P2, then dropped | `apt` has signature verification, a full dependency resolver, and the right `dpkg` order |
| Pin `libc6` with a local repo | Not for glibc: glibc uses version pinning instead | dn-glibc sits outside `dpkg`; no glibc package needs distributing through a repo |
| A dedicated `ld.so.cache` for the runtime | Dropped. Uses the tree's own `/etc/ld.so.cache` | The cache `ldconfig` builds in the tree already holds in-tree paths |
| `init.sh` as the first process | `dn-trace` is the first process; `init.sh` is its child | A filter returning `TRACE` needs the tracer present from the start |

## Components and boot flow

Four components. `dn-*` names are provisional. `TREE` is the Debian tree's
root directory; `RT` is the overlay's own directory inside `TREE` that `dpkg`
does not manage (outside its package database and control, not outside the
filesystem). `TREE` (with `RT` inside it) ships as one tarball
([`prefix.md`](prefix.md)).

| Component | What it is | Its job |
| --- | --- | --- |
| dn-policy | A static C library | The path-mapping table, in-tree symlink resolution, reverse translation, fake root, hardlinks. Never runs on its own; linked into both dn-glibc and dn-trace |
| dn-glibc | glibc and `ld.so`, built from Debian source, placed in `RT` | The fast path: calls dn-policy before every path-taking syscall, then calls the kernel through the fixed gate page |
| dn-trace | The tracer, the tree's root process | Installs the shared filter for the first child; runs the exec gate; handles every caught call via `ptrace`, through dn-policy |
| init.sh | The tree's shell script | Sets up the environment, completes the prefix on a first boot, then runs the command |

`apt` and `dpkg` run in the tree like any other program (see
[apt and dpkg](#apt-and-dpkg-under-the-runtime)).

### Boot order

1. The host runs the contract's `entry` from the prefix root
   ([`prefix.md`](prefix.md), "Boot"): `dn-trace` with the runtime loader, the
   catalog, and `RT/init.sh`.
2. `dn-trace` forks a child, marked traced (fork, vfork, clone and exec all
   tracked). The child installs the shared filter itself, then execs the
   prefix's init. The filter propagates to every later process.
3. That `exec` already passes through the exec gate. The init runs under the
   tree's own shell, so from this step on it runs through dn-glibc.
4. init sets the environment (env vars, `TREE/tmp`, `resolv.conf`,
   `policy-rc.d`) and, on a first boot, completes the prefix from
   `.dn/profile` itself; then it execs the command it was given, or an
   interactive shell.

Every process in the tree is a descendant of `dn-trace`. Nothing execs into
the tree from outside.

## The exec gate

Every `execve` and `execveat` in the tree stops for `dn-trace`. It reads the
target file, classifies it by the first matching rule below, then rewrites the
exec arguments in the process's memory before letting the kernel continue.
Classification is purely from the file header — no trial run.

| Order | Signature | Class | Exec rewritten to |
| --- | --- | --- | --- |
| 1 | Starts with `#!` | Script | The interpreter path, translated through dn-policy, then put through the gate again from rule 2 |
| 2 | ELF with no `PT_INTERP` | Static, including static Go | Left as is, only its own path translated. Its calls fall to `ptrace` on their own |
| 3 | `PT_INTERP` is the standard glibc loader (`/lib/ld-linux-aarch64.so.1`) | Dynamic glibc, including Go with cgo | `RT/ld.so --argv0 <original argv0> <real path> <args>` |
| 4 | `PT_INTERP` names another loader (AppImage, conda, Electron, musl) | Foreign | `<its own loader, translated> <real path> <args>`. Its calls fall to `ptrace` on their own |
| 5 | Not an ELF, not a script | Not runnable | Returns `ENOEXEC`, as the kernel would |

Rules that go with this:

- Every `PT_INTERP`-bearing ELF must be exec'd through the loader explicitly.
  The kernel looks for the path in `PT_INTERP` on Android's real filesystem,
  not in the tree, so a plain exec would fail `ENOENT`. This is why a package
  keeps its stock Debian `PT_INTERP`: the gate, not the file, picks the loader.
- No Go detection. Go-with-cgo's C side runs natively; Go's own runtime makes
  its own syscalls, outside the gate page, so it falls to `ptrace` on its own.
- Running through `RT/ld.so` makes `/proc/self/exe` point at the loader.
  dn-policy answers `readlink("/proc/self/exe")` with the real program's path.
- Packages in the tree no longer need `PT_INTERP` patched. A foreign program
  loaded from outside takes the same path, no exception.
- Caching the classification is optional, keyed by (device, inode, mtime). A
  script that spawns many children pays the classification cost on every exec,
  so this cache belongs in P4.

## The seccomp filter and its tiers

The whole tree uses **one shared filter**, installed once at the root. Whether
a call is caught depends on **where it originates**, not which program made it
— so there is no separate "adoption" step: a static, Go, or foreign program
falls to `ptrace` on every call on its own, while a dynamic glibc program
takes the native path.

### The fixed gate page

The filter recognizes a dn-glibc call by instruction address. glibc's address
changes every run, so a fixed gate page is used instead:

- On first run, dn-glibc's loader maps a 4 KB page holding the kernel-call
  instruction at a fixed address, `P_GATE`, using `MAP_FIXED_NOREPLACE`.
- Every syscall dn-glibc and its loader make goes through this page; never
  calls the kernel directly elsewhere.
- `P_GATE` must sit in 39-bit address space, since many Android arm64 kernels
  only have 39 bits. It is `0x100000000` (4 GiB), where `svc #0; ret` is
  written and the page made `r-x`.
- If the mapping fails (the address is already taken), the program still runs,
  just with every one of its calls falling to `ptrace`. Correct, but slow.
- This is not a security boundary. A program that deliberately jumps into the
  gate page only breaks itself.

### The shared filter's rules

| Call | Issued from the gate page | Issued from elsewhere |
| --- | --- | --- |
| `execve`, `execveat` | TRACE (exec gate) | TRACE (exec gate) |
| The path group and identity group (in the catalog) | ALLOW | TRACE |
| Everything else | ALLOW | ALLOW |

The list is for arm64; adding x86_64 later needs its own additions. The exact,
categorized list is [`src/syscalls.tsv`](../../src/syscalls.tsv)
([below](#the-syscall-catalog)).

### Processing tiers, cheapest first

| Tier | Where it's handled | Applies to | Phase |
| --- | --- | --- | --- |
| 0 | dn-glibc: dn-policy translates, then the call goes through the gate page | Calls a dynamic glibc program makes through glibc | P2 |
| 1 | In-kernel BPF returns ALLOW | Everything outside the two groups; any call from the gate page | P1 |
| 2 (optional) | Seccomp user notification, `dn-trace` holds the listener | Hot calls outside the gate: `openat` via `ADDFD` (kernel ≥ 5.9), the stat group | P5 |
| 3 | `ptrace` in `dn-trace`, via dn-policy | Every exec; every other call outside the gate: static, Go, foreign, or a dynamic program's own stray call | P1 (exec), P3 (the rest) |

Rules that go with this:

- **No per-process `SIGSYS` trap.** The filter survives `exec`: a dynamic
  program exec'ing a static one would hand it `SIGSYS` and kill it.
- **glibc's own `syscall()` function** also goes through dn-policy for numbers
  in the two groups, since it calls through the gate page and would otherwise
  be let through unchecked.
- **Translation must not repeat.** A call dn-glibc already translated can
  still reach `ptrace` (for example, if mapping the gate page failed).
  dn-policy leaves a path already under `TREE` or `RT` unchanged.
- **A program installing its own extra seccomp filter** (a browser sandbox):
  the kernel applies the strictest result across filters. Accepted.
- **`io_uring`** bypasses every tier. Android currently blocks it for ordinary
  apps, so this is not yet handled.

## dn-policy

dn-policy is the only place rules live. dn-glibc and dn-trace only call it;
neither has rules of its own. It lives in `src/dn-policy/` and is a static
library linked into both.

### Path rewriting

- **Prefix mapping, longest match wins.** `/` maps to `TREE`. Exceptions go
  straight to the real system: `/proc`, `/sys`, `/dev`. The exception list
  lives in a config file.
- **No double translation.** A path already under `TREE` or `RT` is left as is.
- **Relative paths** are left for the kernel to resolve, since its
  current-directory is already a real path. Exception: a `..` that would climb
  above `TREE`'s root must be blocked there.
- **Absolute in-tree symlinks** (such as the ones in `/etc/alternatives`)
  point against the tree's own root, but the kernel resolves them against
  Android's real root. dn-policy resolves each path component itself and
  rewrites an absolute symlink target into `TREE`. The last component is not
  resolved when the call carries `O_NOFOLLOW` or `AT_SYMLINK_NOFOLLOW`. The
  resolution result is cached, invalidated for a directory when it changes.
- **Reverse translation** for whatever the kernel returns as a real path:
  `getcwd`, and the links under `/proc/self/` (`cwd`, `fd/N`, `exe`).
  `readlink` on an in-tree symlink needs no reverse translation, since its
  target is already stored as an in-tree path.
- **`/proc/self/exe`** returns the real program's path, not the loader's.
  dn-glibc records this path when the loader starts.
- **UNIX sockets:** a sockaddr path is capped at 108 bytes, and the real path
  is long. Keep `TREE` as short as possible. Past 108 bytes, bind or connect
  with a relative path from the parent directory, through `/proc/self/fd/N`.

### Fake root

- The `get*id` group returns 0. The `set*id` group records the new value and
  reports success.
- `chown`, `fchown`, `fchownat` never touch the real file; they only record
  the new owner in an owner store. `chmod`'s setuid/setgid bits go into the
  same store.
- The stat group's result gets `st_uid`, `st_gid`, and the permission bits
  rewritten from the store. A file absent from the store reports uid 0, gid 0
  — as an ordinary Debian install would.
- `mknod` for a device file creates an empty regular file and records the
  device type in the store.
- **The owner store:** prefers the `user.dn.*` xattr on the file itself, since
  it needs no locking and travels with the file across a rename. Tested
  on-device (Termux's data partition accepts `user.*` xattrs round-trip); a
  database file in `RT/state/`, keyed by (device, inode), is the fallback
  (`DN_POLICY_OWNER_BACKEND=db`), with file locking since multiple processes
  write it.
- **`security.*` xattrs:** some packages call `setcap` in their install script
  (`ping` setting `cap_net_raw`). That needs real privilege; failing it marks
  the package broken. dn-policy intercepts `setxattr`, `getxattr`,
  `removexattr` for `security.*` names, writes to the owner store, and reports
  success.

### Hardlinks

SELinux on Android blocks `link()` in app data, which `dpkg` needs when
unpacking a package containing hardlinks. Uses PRoot's `link2symlink`
approach:

- The original file is moved into a hidden file.
- Both the old and new names become symlinks to that hidden file.
- A link count travels with it; only removing the last name removes the hidden
  file.
- The stat group must report these names as regular files with the right link
  count, not as symlinks.

## dn-glibc

dn-glibc always matches the version of `libc6` installed in the tree. That is
the only coupling between the runtime and the package set.

### Building dn-glibc

- Source: Debian's own `glibc` source package, at the exact version of the
  tree's `libc6`. Two patches apply, in order: the Android base
  (`patches/dn-glibc-android.patch`) and the dn-policy wiring
  (`patches/dn-policy-glibc-wiring.patch`), applied by
  `scripts/glibc/dn-apply-glibc-patch.sh`. Never hand-edited.
- The wiring does four things:
  1. Wires dn-policy into every path-taking function — the public version, the
     internal version, and `syscall()`.
  2. Routes every kernel call glibc makes through the gate page
     (`INTERNAL_SYSCALL_RAW`, `syscallS.S`).
  3. Has the loader map the gate page at startup, before opening any file.
  4. Has the loader search libraries in `RT/lib` first, then the tree's own
     library directories, using the tree's own `ld.so.cache`.
- The patched libraries ship as the `libc6` package and land at the tree's
  normal multiarch path, so the whole glibc-source runtime set is the patched
  build. (`RT/lib` — the wiring's first search dir — is a reserved slot; the
  shipped packages leave it empty.)
- Data files (gconv, locale) come from the tree. Both sides share a version, so
  they are compatible.
- The package's `+dn<n>` version is the version marker; a new glibc version
  means a new build of the package. It is keyed only to (CPU architecture,
  `libc6` ABI version), so the same `.deb` is reused across every tarball
  pinned to that version.

### What the wiring covers

The wiring is per syscall, in `src/syscalls.tsv`'s terms. Currently wired
in-process: the open/openat family; stat/fstatat/statx/faccessat plus the owner
rewrite; `getcwd` and `readlink(/proc/self/...)` reverse translation;
`mkdir`/`rmdir`/`rename{,at,at2}`/`symlink`/`truncate`/`utimensat`/`statfs`;
`syscall(2)` for the path group; `chown`/`lchown`/`chmod`/`fchmodat`;
`link`/`unlink` (link2symlink). The loader maps the gate page and searches
`RT/lib` first.

Still on the `ptrace` tier (correct, just slower): the `syscalls.list`-generated
wrappers (`mkdirat`/`unlinkat`/`symlinkat`/`linkat`/`readlinkat`/`fchownat`/
the `*xattr` family) and any direct static/foreign call. The cancelable path
(`syscall_cancel.S`) is the last piece before every glibc syscall is issued
from the gate page.

## The syscall catalog

[`src/syscalls.tsv`](../../src/syscalls.tsv) is the single source of truth: one
row per syscall, with its group, handling, whether dn-glibc handles it
in-process, and whether its kernel call is issued from the gate page. It
carries its own provenance (architecture, Android/kernel scope, sources,
updated), because both the list and the handling follow the Android kernel and
policy and drift over time.

`dn-trace` builds the filter's gate-IP exemption from it (`--syscalls`), so it
never maintains a list of its own; `tools/check-syscalls.py` checks it.

## apt and dpkg under the runtime

`apt` and `dpkg` are dynamic glibc programs, so they run unmodified through the
runtime. No `--root` or `--admindir`: `/var/lib/dpkg` and `/var/cache/apt`
land in the tree on their own. A package's install script runs under fake root.
Points already settled:

- **The `ldconfig` trigger:** Debian's `/sbin/ldconfig` is static, so it takes
  the `ptrace` path. Slow but correct, and it runs once per install batch.
- **Permission checks:** `dpkg` needs root; `apt` drops to `_apt` and checks
  the owner of the `partial` directory. Both go through fake root, so they
  agree.
- **Creating a user:** `adduser` edits the tree's `/etc/passwd`; glibc rereads
  it through NSS.
- **`setcap`:** handled by the fake `security.*` xattr.
- **Starting a service on install:** blocked with `policy-rc.d`.
- **apt's own seccomp sandbox:** usually doesn't touch the shared filter. If an
  unusual download fails, set `APT::Sandbox::Seccomp "false";`.

Because a `.deb` installs intact (no translation hook), the old apt/dpkg hooks
are gone: the runtime is the only thing between a package and the kernel.

### The glibc rule

The patched glibc ships as **real Debian packages** — `libc6` and `libc-bin`,
built from the tree's `libc6` source with the two patches, at version
`<Debian version>+dn<n>` (`2.41-12+deb13u4+dn1`) so it is plain they are not
upgradeable from the mirror. They are delivered through the **local repo**
(below): its origin pin (priority 1001) beats the mirror (500), so `apt` can
never replace them. There is no separate version-pin file, and `apt-mark hold`
is not used. The tree ships only the runtime pair; Debian's version-pinned dev
packages (`libc6-dev`, `libc-dev-bin`, `locales`) are out of scope, and if one
is ever shipped the whole glibc source binary set is rebuilt at the same
`+dn<n>` together.

### The local repo

The local repo is shipped inside `RT` (`RT/repo`, outside the dpkg-managed
tree) and always wins over the mirror:

- The patched glibc packages (`libc6`, `libc-bin`), at `+dn<n>`.
- Dynamic builds standing in for a static or Go program in everyday use.
- `equivs`-built dummy packages, to satisfy a dependency the tree can't use.
- Packages patched to run on Android, where the runtime can't cover it alone.

```
# /etc/apt/sources.list.d/dn-local.list   (the path is a guest path)
deb [trusted=yes] file:/usr/lib/deb-native/repo ./

# /etc/apt/preferences.d/dn-local
Package: *
Pin: release o=deb-native
Pin-Priority: 1001
```

The repo's `Release` carries `Origin: deb-native`; the pin recognizes it. The
mirror's own `libc6`/`libc-bin` are simply shadowed — the tree's `libc6` at
`+dn<n>` is the candidate apt picks, at priority 1001. Rules: the pin only
applies to a package the repo carries; a whole source package's binary set
goes in together; a rebuilt package's version is `<Debian version>+dn<n>` (the
glibc pair included); a check script lists local-repo packages the mirror has
a newer version of.

## Init, services, and hard limits

### What init.sh does

1. Sets environment variables (`PATH`, `HOME`, `TMPDIR`). No `LD_PRELOAD` or
   `LD_LIBRARY_PATH` needed.
2. Creates the tree's `tmp` with mode 1777.
3. Writes the tree's `etc/resolv.conf` when the artifact did not link it.
   Android provides no such file, and without it no glibc program can resolve
   a domain name.
4. Sets `usr/sbin/policy-rc.d` to return 101 — Debian's standard mechanism to
   stop an install script from starting a service on its own.
5. On a first boot (no `.dn/bootstrapped`), completes the prefix from
   `.dn/profile`.
6. `exec`s the command it was given, or an interactive shell.

### Services

- Each service is a directory under `/etc/service` with a hand-written `run`
  file. A package's own systemd unit is never used.
- No systemd, no system D-Bus. A program that genuinely needs either is out of
  scope.
- **The phantom process killer** (Android 12+) kills child processes once an
  app spawns past roughly 32; the tracer, `runsvdir` and its services hit this
  easily. Disable it over `adb`:
  - Android 12L+: `settings put global settings_enable_monitor_phantom_procs false`
  - Android 12: `device_config put activity_manager max_phantom_processes 2147483647`

### Hard limits, out of scope

| Can't do | Why |
| --- | --- |
| Listen on a port below 1024 | Needs real root; fake root isn't enough |
| Mount, namespaces, containers | Needs real root |
| Read `/proc/net` | Android 10+ blocks it for ordinary apps |
| System systemd, system D-Bus | Not part of this design |
| A display (X11, Wayland), GPU | Needs a separate component, not designed yet |
| `io_uring` | Bypasses every tier; Android already blocks it |

## Status

Implemented and exercised on-device:

- **dn-policy** (`src/dn-policy/`) — the mapping, symlink resolution, reverse
  translation, fake root and hardlinks.
- **dn-glibc** — the two patches build; the gate page is mapped; `RT/lib`
  search order; the path group wired in-process (see
  [What the wiring covers](#what-the-wiring-covers)).
- **dn-trace** — the tree's root; the shared filter with the gate-IP rule built
  from the catalog; the exec gate rule 3 (`--rt-loader`).
- **`src/syscalls.tsv`** — the catalog, checked by `tools/check-syscalls.py`.

Still open (P2/P3):

- Exec-gate rule 4 (a foreign loader) and its argv semantics.
- Issue **every** glibc syscall from the gate page: the cancelable path
  (`syscall_cancel.S`) is the delicate one — its `_arch_start`/`_end`
  cancellation markers assume the svc is inline.
- Turn on the path and identity groups in the shared filter (P3): `dn-trace`
  handles them via `ptrace` through dn-policy, replacing the old `ptrace`
  mechanism.
- The `bind`/`connect` UNIX-socket path (sockaddr, 108-byte cap).
- Route the `syscalls.list`-generated wrappers through dn-policy.
- Version pinning generated by the build (see
  [The glibc rule](#the-glibc-rule)).
