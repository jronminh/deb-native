# Runtime: dn-policy, dn-glibc, dn-trace

<!-- template: templates/docs.template.md -->

How a prefix runs stock Debian `.deb` packages intact, once installed:
every path-touching syscall resolves through one shared policy, dynamic
glibc binaries take a native-speed fast path, everything else (static,
Go, foreign loaders such as AppImage/conda/Electron/musl) falls back to a
traced path with identical results. Runtime speed is the priority;
install time may be slow. Status: **design only, nothing implemented**.
This replaces the whole existing overlay — `dn-shim.c` (`LD_PRELOAD`
path redirection), `dn-run`'s adopt-on-first-run, `dn-elf`'s run-time
interpreter rewriting, and `patch-maintainer-scripts.sh`'s per-script
`sed` rewrite — rather than running alongside it.

## Contents

- [Goal and principles](#goal-and-principles)
- [What this replaces](#what-this-replaces)
- [Components and boot flow](#components-and-boot-flow)
- [The exec gate](#the-exec-gate)
- [The seccomp filter and its tiers](#the-seccomp-filter-and-its-tiers)
- [dn-policy](#dn-policy)
- [glibc: build, versioning, and packages](#glibc-build-versioning-and-packages)
- [Init, services, and hard limits](#init-services-and-hard-limits)
- [Roadmap and open questions](#roadmap-and-open-questions)

## Related docs

- [`prefix-contract.md`](prefix-contract.md) — the `.dn/` install contract.
  Its relocation mechanism (byte-patching `PT_INTERP` with `dn-elf`,
  `.dn/baked-paths`, installing into a host-chosen directory) is dropped
  under this design: `TREE`/`RT` are fixed at build time, and the exec gate
  resolves the right loader on every `execve` instead, so no file ever
  needs its interpreter rewritten after the build. The rest of that
  contract (`.dn/contract`, layout, what the host does) is unaffected.
- [`prefix-layers.md`](prefix-layers.md) — core-ultra, core-deb and
  specialized prefixes. All three keep applying: dn-policy hardcodes
  nothing prefix-specific (see principle 5 below), so one build of
  dn-policy/dn-glibc/dn-trace per (CPU arch, `libc6` ABI version) is
  reused across every layer's tarball on that version, not rebuilt per
  layer.
- [`dn-glibc-prefix.md`](dn-glibc-prefix.md) — the shipped glibc patch
  (the "10-file swap": reuse Debian's own `libc6`/`libc-bin`, patch only
  the loader and cache paths to self-derive the prefix). This design
  replaces it with a full rebuild of glibc from Debian source, because
  dn-policy must be wired into every path-taking function, public and
  internal, which the 10-file swap's narrow patch surface cannot reach —
  internal glibc-to-glibc calls bypass the PLT and no `LD_PRELOAD`
  interposition can intercept them.
- [`tracer/tracer.md`](tracer/tracer.md) — the current `dn-trace`, invoked
  by `dn-run` only for a static binary or a raw syscall. This design makes
  `dn-trace` the root process of the whole tree instead, with one seccomp
  filter installed once, so every process is a descendant of it.
- [`shim/path-shim.md`](shim/path-shim.md) — the current `dn-shim.c`
  (`LD_PRELOAD` symbol interposition) and the maintainer-script `sed`
  rewrite it needed on top of itself, because interposition cannot reach
  a script's literal paths or glibc's own internal calls. dn-policy,
  wired into glibc at the source level, reaches both.

## Goal and principles

Goal: install stock Debian `.deb` packages into a tree without modifying
them. A dynamic glibc program runs at native speed; everything else falls
back to a traced path with the same result. Runtime speed is the
priority; install may be slower.

Five principles, everything below follows them:

1. **No per-package patching.** Every adjustment lives in the runtime and
   applies by syscall class, not by package.
2. **The unit of decision is the individual syscall.** Classifying a
   program only picks the initial filter.
3. **One policy, enforced in two places.** The rules for path rewriting,
   fake root, and hardlinks live in one shared library (dn-policy). The
   glibc runtime (fast path) and the tracer (fallback path) both call it,
   so the two paths can never disagree.
4. **Push each call to the cheapest tier that can handle it.** Optimize
   the distribution of calls across tiers, not each tier in isolation.
5. **dn-policy hardcodes nothing prefix-specific.** Every difference
   between prefixes (core-ultra, core-deb, specialized) is data read at
   run time, not something rebuilt. `TREE`/`RT` self-derive from the
   loader's own path (as glibc's prefix self-derivation already does);
   the path-exception list and the owner-store backend choice live in a
   config file or get probed at startup; `P_GATE` is a device/kernel
   property (probed once, stored), not a prefix property. Consequence:
   dn-policy, dn-glibc, dn-trace, and the seccomp filter need only one
   build per (CPU architecture, `libc6` ABI version), reused unmodified
   across every core-ultra/core-deb/specialized tarball on that version —
   never rebuilt per prefix. The one thing legitimately hardcoded is the
   architecture's syscall-number list (below), since it is an ABI
   property identical across every prefix on one architecture.

## What this replaces

| Prior approach or discussion | This design | Why |
| --- | --- | --- |
| Patch `PT_INTERP` and rpath per package | Dropped. The exec gate switches the loader at exec time | No more per-package patching; foreign programs are covered too |
| `LD_PRELOAD` path-redirect shim | Dropped. Path rewriting lives in dn-glibc | Survives a program clearing its environment; reaches glibc's own internal calls and `dlopen`, which interposition cannot |
| First-run trial ("adopt everything") to classify a program | Dropped. Classification is static, at exec time; whether a call is caught depends only on where it originates | One run never proves later runs; the filter catches a mismatch the moment it happens |
| Adoption stub installs a broad filter per process; the loader installs a filter keyed to glibc's address | Dropped. One shared filter, installed once at the root, recognizes dn-glibc via a fixed gate page (below) | The filter survives `exec`; installing one per process stacks up and misapplies addresses to child programs |
| Trap `SIGSYS` inside the process | Dropped | A static program exec'd afterward would receive `SIGSYS` and die |
| Separate Go detection | Dropped | Static Go falls into the static group; Go with cgo runs through dn-glibc, and its own direct syscalls fall to ptrace |
| A hand-written resolver in place of `apt` | Kept temporarily through P2, then dropped. `apt` runs unmodified through the runtime | `apt` has archive signature verification, a full dependency resolver, and the right `dpkg` call order |
| Pin `libc6` with a local repo | Not used for glibc: glibc uses version pinning instead. The local repo still exists for Android-specific rebuilds | dn-glibc sits outside `dpkg`; no glibc package needs distributing through a repo |
| A dedicated `ld.so.cache` for the runtime | Dropped. Uses the tree's own `/etc/ld.so.cache` | The cache `ldconfig` builds inside the tree already holds in-tree paths |
| `init.sh` as the first process | `dn-trace` is the first process; `init.sh` is its child | A filter returning `TRACE` needs the tracer present from the start |

## Components and boot flow

Runtime has four components. `dn-*` names are provisional. `TREE` is the
Debian tree's root directory; `RT` is the runtime's own directory, a
subdirectory inside `TREE` that `dpkg` does not manage (outside its
package database and control, not outside the filesystem). `TREE`
(with `RT` inside it) ships as one tarball, same artifact model as
[`prefix-layers.md`](prefix-layers.md)'s core-ultra/core-deb/specialized
layers.

| Component | What it is | Its job |
| --- | --- | --- |
| dn-policy | A static C library | The path-mapping table, in-tree symlink resolution, reverse translation, fake root, hardlinks. Never runs on its own; linked into both dn-glibc and dn-trace |
| dn-glibc | glibc and `ld.so`, built from Debian source, placed in `RT` | The fast path: calls dn-policy before every path-taking syscall, then calls the kernel through the fixed gate page (below) |
| dn-trace | The tracer, the tree's root process | Installs the shared filter for the first child process; runs the exec gate; handles every caught call via `ptrace`, through dn-policy |
| init.sh | The existing shell script | Sets up the tree's environment, ends with `exec runsvdir` |

`apt` and `dpkg` run in the tree like any other program (see
[glibc: build, versioning, and packages](#glibc-build-versioning-and-packages)).

### Boot order

1. The user runs `dn-trace` from Termux.
2. `dn-trace` forks a child, marked traced (fork, vfork, clone and exec all
   tracked). The child installs the shared filter itself, then execs
   `init.sh`. The filter propagates to every later process.
3. That `exec` of `init.sh` already passes through the exec gate. The
   tree's shell is a dynamic ELF, so it runs through dn-glibc from this
   step on.
4. `init.sh` sets up the environment: env vars, `TREE/tmp`, state
   directories.
5. `init.sh` execs `runsvdir`; each service is a directory with a `run`
   file.

Every process in the tree is a descendant of `dn-trace`. Nothing execs
into the tree from outside.

## The exec gate

Every `execve` and `execveat` in the tree stops for `dn-trace`. It reads
the target file, classifies it by the first matching rule below, then
rewrites the exec arguments in the process's memory before letting the
kernel continue. Classification is purely from the file header — no trial
run.

| Order | Signature | Class | Exec rewritten to |
| --- | --- | --- | --- |
| 1 | Starts with `#!` | Script | The interpreter path, translated through dn-policy, then put through the gate again from rule 2 |
| 2 | ELF with no `PT_INTERP` | Static, including static Go | Left as is, only its own path translated. Its calls fall to `ptrace` on their own (below) |
| 3 | `PT_INTERP` is the standard glibc loader (`/lib/ld-linux-aarch64.so.1`) | Dynamic glibc, including Go with cgo | `RT/ld.so --argv0 <original argv0> <real path> <args>` |
| 4 | `PT_INTERP` names another loader (AppImage, conda, Electron, musl) | Foreign | `<its own loader, translated> <real path> <args>`. Its calls fall to `ptrace` on their own |
| 5 | Not an ELF, not a script | Not runnable | Returns `ENOEXEC`, as the kernel would |

Rules that go with this:

- Every `PT_INTERP`-bearing ELF must be exec'd through the loader
  explicitly. The kernel looks for the path in `PT_INTERP` on Android's
  real filesystem, not in the tree, so a plain exec would fail `ENOENT`.
- No Go detection needed. Go-with-cgo's C side runs natively; Go's own
  runtime makes its own syscalls, outside the gate page, so it falls to
  `ptrace` on its own.
- Running through `RT/ld.so` makes `/proc/self/exe` point at the loader.
  dn-policy must answer `readlink("/proc/self/exe")` with the real
  program's path instead (see [dn-policy](#dn-policy)).
- Packages in the tree no longer need `PT_INTERP` patched. A foreign
  program loaded from outside takes the same path, no exception.
- Caching the classification is optional, keyed by (device, inode,
  mtime). A script that spawns many child processes (`configure`, `make`,
  a shell loop) pays the classification cost on every exec, so this cache
  belongs in P4.

## The seccomp filter and its tiers

The whole tree uses **one shared filter**, installed once at the root.
Whether a call is caught depends on **where it originates**, not which
program made it — so there is no separate "adoption" step: a static, Go,
or foreign program falls to `ptrace` on every call on its own, while a
dynamic glibc program takes the native path.

### The fixed gate page

The filter recognizes a dn-glibc call by instruction address. glibc's
address changes every run, so a fixed gate page is used instead:

- On first run, dn-glibc's loader maps a 4 KB page holding the kernel-call
  instruction at a fixed address, `P_GATE`, using `MAP_FIXED_NOREPLACE`.
- Every syscall dn-glibc and its loader make goes through this page;
  never calls the kernel directly elsewhere.
- `P_GATE` must sit in 39-bit address space, since many Android arm64
  kernels only have 39 bits.
- If the mapping fails (the address is already taken), the program still
  runs, just with every one of its calls falling to `ptrace`. Correct, but
  slow.
- This runtime is not a security boundary. A program that deliberately
  jumps into the gate page only breaks itself.

### The shared filter's rules

| Call | Issued from the gate page | Issued from elsewhere |
| --- | --- | --- |
| `execve`, `execveat` | TRACE (exec gate) | TRACE (exec gate) |
| The path group and identity group (below) | ALLOW | TRACE |
| Everything else | ALLOW | ALLOW |

List is for arm64. No legacy calls such as `open` or `stat`; adding
x86_64 later needs its own additions.

- **Path group:** `openat`, `openat2`, `faccessat`, `faccessat2`,
  `newfstatat`, `statx`, `readlinkat`, `mkdirat`, `mknodat`, `unlinkat`,
  `renameat`, `renameat2`, `linkat`, `symlinkat`, `chdir`, `chroot`,
  `fchmodat`, `fchmodat2`, `truncate`, `utimensat`, `statfs`, the
  `*xattr` family keyed by filename, `inotify_add_watch`, `getcwd`,
  `bind` and `connect` (a UNIX socket's path lives in the sockaddr).
- **Identity group:** `getuid`, `geteuid`, `getgid`, `getegid`,
  `getresuid`, `getresgid`, `getgroups`, the `set*id` family, `fchown`,
  `fchownat`, `fstat`. `fstat` is here because fake root must rewrite the
  owner fields in its result.

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
  Installing a filter per process also stacks across each `exec` until it
  hits the kernel's limit.
- **glibc's own `syscall()` function** also goes through dn-policy for
  numbers in the two groups, since it calls through the gate page and
  would otherwise be let through unchecked.
- **Translation must not repeat.** A call dn-glibc already translated can
  still reach `ptrace` (for example, if mapping the gate page failed).
  dn-policy must leave a path already under `TREE` or `RT` unchanged.
- **A program installing its own extra seccomp filter** (a browser
  sandbox, say): the kernel applies the strictest result across filters.
  Their filter may block a call before it reaches `dn-trace`. Accepted.
- **`io_uring`** bypasses every tier. Android currently blocks it for
  ordinary apps, so this is not yet handled.

## dn-policy

dn-policy is the only place rules live. dn-glibc and dn-trace only call
it; neither has rules of its own.

### Path rewriting

- **Prefix mapping, longest match wins.** `/` maps to `TREE`. Exceptions
  go straight to the real system: `/proc`, `/sys`, `/dev`. The exception
  list lives in a config file.
- **No double translation.** A path already under `TREE` or `RT` is left
  as is.
- **Relative paths** are left for the kernel to resolve, since its
  current-directory is already a real path. Exception: a `..` that would
  climb above `TREE`'s root must be blocked there.
- **Absolute in-tree symlinks** (such as the ones in `/etc/alternatives`)
  point against the tree's own root, but the kernel resolves them against
  Android's real root. dn-policy must resolve each path component itself
  and rewrite an absolute symlink target into `TREE`. The last component
  is not resolved when the call carries `O_NOFOLLOW` or
  `AT_SYMLINK_NOFOLLOW`. The resolution result is cached, invalidated for
  a directory when that directory changes.
- **Reverse translation** for whatever the kernel returns as a real path:
  `getcwd`, and the links under `/proc/self/` (`cwd`, `fd/N`, `exe`).
  `readlink` on an in-tree symlink needs no reverse translation, since its
  target is already stored as an in-tree path.
- **`/proc/self/exe`** returns the real program's path, not the loader's.
  dn-glibc records this path when the loader starts.
- **UNIX sockets:** a sockaddr path is capped at 108 bytes, and Termux's
  real path is long. Keep `TREE` as short as possible. Past 108 bytes,
  bind or connect with a relative path from the parent directory, through
  `/proc/self/fd/N` of that directory.

### Fake root

- The `get*id` group returns 0. The `set*id` group records the new value
  and reports success.
- `chown`, `fchown`, `fchownat` never touch the real file; they only
  record the new owner in an owner store. `chmod`'s setuid/setgid bits go
  into the same store.
- The stat group's result gets `st_uid`, `st_gid`, and the permission
  bits rewritten from the store. A file absent from the store reports
  uid 0, gid 0 — as an ordinary Debian install would.
- `mknod` for a device file creates an empty regular file and records the
  device type in the store.
- **The owner store:** prefers the `user.dn.*` xattr on the file itself,
  since it needs no locking and travels with the file across a rename.
  Must be tested on-device whether Termux's data partition allows writing
  user xattrs. If not, fall back to a database file in `RT/state/`, keyed
  by (device, inode), with file locking since multiple processes write it;
  an entry is removed when the file's last link is removed.

**`security.*` xattrs:** some packages call `setcap` in their install
script (`ping` setting `cap_net_raw`, for instance). That needs real
privilege; failing it marks the package broken. dn-policy intercepts
`setxattr`, `getxattr`, `removexattr` for `security.*` names, writes to
the owner store, and reports success. The program still doesn't have that
privilege for real, but the package finishes installing clean.

### Hardlinks

SELinux on Android blocks `link()` in app data, which `dpkg` needs when
unpacking a package containing hardlinks. Uses PRoot's `link2symlink`
extension's approach:

- The original file is moved into a hidden file.
- Both the old and new names become symlinks to that hidden file.
- A link count travels with it; only removing the last name removes the
  hidden file.
- The stat group must report these names as regular files with the right
  link count, not as symlinks.

## glibc: build, versioning, and packages

dn-glibc always matches the version of `libc6` installed in the tree.
That is the only coupling between the runtime and the package set.

### Building dn-glibc

- Source: Debian's own `glibc` source package, at the exact version of
  the tree's `libc6`. The fix is a fixed patch set applied to the source,
  never hand-edited.
- The patch does four things:
  1. Wires dn-policy into every path-taking function — the public
     version, the internal version, and `syscall()`.
  2. Routes every kernel call glibc makes through the gate page.
  3. Has the loader map the gate page at startup, before opening any
     file.
  4. Has the loader search libraries in `RT/lib` first, then the tree's
     own library directories, using the tree's own `ld.so.cache` (the one
     `ldconfig` in the tree builds). This cache holds in-tree paths,
     which dn-policy translates on open.
- `RT/lib` holds every library built from glibc source (`libc.so.6`,
  `libm.so.6`, `libresolv`, `libnss_*`, ...), so the tree's own
  same-named `libc6` files are never loaded.
- Data files (gconv, locale) come from the tree. Both sides share a
  version, so they're compatible.
- Installed by version: `RT/glibc-<version>/`, with a `RT/glibc-current`
  symlink that switches in one move, and reverts the same way on failure.
- **Dn-policy hardcodes nothing prefix-specific (principle 5):** this
  build is keyed only to (CPU architecture, `libc6` ABI version), so the
  same `RT/glibc-<version>/` is copied unmodified into every
  core-ultra/core-deb/specialized tarball pinned to that version — never
  rebuilt per prefix. Only a genuine `libc6` version mismatch between
  prefixes needs a second build.

### `apt` and `dpkg`

`apt` and `dpkg` are dynamic glibc programs, so they run unmodified
through the runtime. No `--root` or `--admindir`: `/var/lib/dpkg` and
`/var/cache/apt` land in the tree on their own. A package's install
script runs under fake root. Points already settled:

- **The `ldconfig` trigger:** Debian's `/sbin/ldconfig` is static, so it
  takes the `ptrace` path. Slow but correct, and it only runs once per
  install batch.
- **Permission checks:** `dpkg` needs root; `apt` drops to `_apt` and
  checks the owner of the `partial` directory. Both go through fake root,
  so they agree with each other.
- **Creating a user:** `adduser` edits the tree's `/etc/passwd`; glibc
  rereads it through NSS. Nothing extra needed.
- **`setcap`:** handled by the fake `security.*` xattr (above).
- **Starting a service on install:** blocked with `policy-rc.d` (see
  [Init, services, and hard limits](#init-services-and-hard-limits)).
  `deb-systemd-helper` already skips itself with no systemd present.
- **apt's own seccomp sandbox:** usually doesn't touch the shared filter.
  If an unusual package download fails, set
  `APT::Sandbox::Seccomp "false";`.

### The glibc rule

Packages built from the glibc source must never be upgraded past
`RT/glibc-current`'s version. `apt` enforces this with version pinning,
in a file the dn-glibc build script generates itself:

```
# /etc/apt/preferences.d/dn-glibc  (generated, do not hand-edit)
Package: libc6 libc6-dev libc-bin locales ...
Pin: version <RT/glibc-current's version>
Pin-Priority: 1001
```

The package list comes from the `Source: glibc` field in the package
index, not hardcoded. `apt-mark hold` is not used.

### The local repo

The local repo holds packages built specifically to run well on Android,
and always wins over the mirror. A package in the local repo stays at its
version until rebuilt on a newer base — an accepted trade-off.

The repo holds:

- Dynamic builds standing in for a static or Go program in everyday use,
  so it takes the native path.
- `equivs`-built dummy packages, to satisfy a dependency on something the
  tree can't use.
- Packages patched to run on Android, where the runtime can't cover it on
  its own.

```
# /etc/apt/sources.list.d/dn-local.list
deb [trusted=yes] file:/srv/dn-repo ./

# /etc/apt/preferences.d/dn-local
Package: *
Pin: release o=deb-native
Pin-Priority: 1001
```

The repo's `Release` file is generated with `Origin: deb-native`
(`apt-ftparchive -o APT::FTPArchive::Release::Origin=deb-native release .`)
so the pin recognizes it.

Rules:

- The pin only applies to a package the local repo carries. Everything
  else still comes from the mirror as usual.
- A whole source package's binary set goes in together, never a single
  binary alone — packages from one source usually depend on each other at
  an exact version, and a partial set lets the mirror upgrade the rest out
  from under it.
- A rebuilt package's version is `<Debian version>+dn<n>`, so the base it
  was built on is visible at a glance.
- glibc-family packages never go in the local repo; they use version
  pinning above instead.
- A check script lists local-repo packages the mirror already has a newer
  version of, compared with `apt-cache madison`. `apt` won't flag this on
  its own, so this is the only way to know which package — including a
  security-patched one — is due for a rebuild.

### Upgrading glibc

1. `apt update` sees a new `libc6`; the pin holds it back.
2. A script fetches the matching source version, applies the patch,
   builds it into `RT/glibc-<new version>/`.
3. Switches `RT/glibc-current` and regenerates the pin file.
4. `apt upgrade` upgrades the glibc-sourced packages in the tree.

If the patch fails to apply to the new version, it stops at step 2 and
the tree keeps the old version.

## Init, services, and hard limits

### What init.sh does

1. Sets basic environment variables (`PATH`, `HOME`, `LANG`). No more
   `LD_PRELOAD` or `LD_LIBRARY_PATH` needed.
2. Creates `/tmp` in the tree with mode 1777.
3. Writes the tree's `/etc/resolv.conf` with a fixed DNS server, from a
   config file. Android provides no such file, and without it no glibc
   program can resolve a domain name.
4. Sets `/usr/sbin/policy-rc.d` to return code 101 — Debian's standard
   mechanism to stop an install script from starting a service on its
   own; it stands in for faking `systemctl`.
5. `exec runsvdir /etc/service`.

### Services

- Each service is a directory under `/etc/service` with a hand-written
  `run` file. A package's own systemd unit is never used.
- No systemd, no system D-Bus. A program that genuinely needs either is
  out of scope.
- **The phantom process killer** (Android 12+) kills off child processes
  once an app spawns past roughly 32. The tree's tracer, `runsvdir`, and
  its services make this easy to hit. Disable it over `adb`:
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
| Exec'ing a file in app data, if Termux moves to a newer target SDK | Would force a different way to invoke the loader and run a static program |

## Roadmap and open questions

Every phase prioritizes runtime speed; installing packages may be slower.
Each phase leaves the tree runnable. The patched packages and the
existing `ptrace` mechanism for static files stay in use until the phase
that replaces them.

1. **P1, exec gate observe-only.** `dn-trace` is the root process; the
   shared filter only has the exec rule; the gate classifies and logs but
   changes nothing yet.
   Done when: `init.sh` and the existing services run under `dn-trace`
   with no new errors, and a day's real use has a classification log.
2. **P2, the fast path.** dn-policy (mapping, symlinks, reverse
   translation, `/proc/self/exe`, fake root, hardlinks), dn-glibc (gate
   page, library search order), gate rules 3 and 4 turned on. Unpatch a
   few packages first, then all of them. Drop the shim. Switch to `apt`
   plus the glibc pin, drop the hand-written resolver.
   Done when: the packages in use, reinstalled from stock `.deb`s, run
   correctly; `apt update`, `apt install`, `apt upgrade` run end to end
   with no install-script failures.
3. **P3, the fallback path.** Turn on the path and identity groups in the
   shared filter; `dn-trace` handles them via `ptrace` through dn-policy,
   replacing the old `ptrace` mechanism.
   Done when: `busybox-static`, a static Go program, and a Go-with-cgo
   program all run correctly.
4. **P4, measure and optimize.** `dn-trace` counts caught calls by program
   and by number. Optimize `ptrace`: read/write memory with
   `process_vm_readv`/`process_vm_writev`, stop on exit only for a call
   that returns a path, cache path resolution, cache exec-gate
   classification.
   Done when: there's a per-tier call-distribution table for three real
   workloads: installing packages, compiling, running a Python script.
5. **P5, optional.** Seccomp user notification for hot calls, only if
   P4's numbers show `ptrace` taking a significant share.

### Open questions

- [ ] What else does the current shim do besides path redirection? List
      it so everything moves into dn-policy before the shim is dropped in
      P2.
- [x] The existing `ptrace` mechanism becomes `dn-trace`'s core (resolved):
      `src/tracer/` (the pruned PRoot fork) is reused as is for P1 — its
      `proot_sysnums`/`fakeroot_sysnums` lists in `syscall/seccomp.c`
      already match this design's path/identity groups closely, and
      `struct seccomp_data`'s `instruction_pointer` field is already enough
      to add the `P_GATE` ALLOW exception in P2 without a rewrite.
- [ ] The device's kernel version: decides whether P5 (`ADDFD` needs
      ≥ 5.9) is feasible at all.
- [x] Does Termux's data partition allow writing user xattrs (resolved,
      tested directly on-device): yes -- `/tmp`, the Termux home
      directory, and the running prefix's own `tmp` all accept a
      `user.*` `setxattr`/`getxattr`/`removexattr` round-trip. `/sdcard`
      (FUSE/sdcardfs) does not (`ENOTSUP`), and separately lacks
      `flock()` (`ENOSYS`) too -- moot either way, since `TREE` never
      lives there. `dn-policy`'s owner store (`src/dn-policy/
      dn-policy-fakeroot.c`) picks the `user.dn.*` xattr backend by
      probing this at `dn_policy_init()`, with the DB-file fallback
      still implemented and exercisable via `DN_POLICY_OWNER_BACKEND=db`
      for whatever device turns out to need it.
- [x] Choosing `P_GATE` (resolved): `0x100000000` (4 GiB), found free in
      every readable on-device process map and inside the 39-bit range; the
      loader maps it at startup (`elf/rtld.c`, part of
      `patches/dn-policy-glibc-wiring.patch`). Issuing syscalls from it and
      the filter's gate-IP rule are still open (P2).
- [x] Borrowing the idea from PRoot's `link2symlink` is not a licensing
      problem (resolved): this repo is GPL-3.0-or-later (`LICENSE`), and
      PRoot's own code (already vendored in `src/tracer/`) is
      GPL-2.0-or-later -- compatible. dn-policy's hardlink implementation
      is a clean-room reimplementation of the same idea, not a literal
      copy of PRoot's extension source (which isn't in this repo's pruned
      fork to begin with).
- [ ] dn-policy's calling convention isn't defined yet: function
      signatures, error/return conventions, thread safety, and — since
      dn-glibc calls it in-process while `dn-trace` must read/write a
      traced process's memory with `process_vm_readv`/`writev` before and
      after calling it — the wrapper that makes the two call sites agree.
