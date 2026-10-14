# TODO / roadmap

Current truth only. Finished work lives in the specs
([`docs/spec/`](docs/spec/)); this file is what is still open.

## Alpha goal (the next announcement)

**Goal**: pre-alpha means "works for the author"; alpha means "try it, it
mostly works". Reached when everything below is done. Announce with one
number ("N of 100 random Debian packages install and run, no root, no
proot"), the comparison with proot-distro (README), and a `gcc` demo.

**Status**: pre-alpha. A shipped prefix runs real Debian glibc packages
unprivileged; `apt install gcc` compiles and runs end to end.

**Open**:

- **Popular languages**: `python3` + a C-extension package, `perl` + an XS
  module, `ruby`, `nodejs` — the Perl version gap included (a package builds
  modules for one Perl ABI; the prefix's `perl` must match).
- **Unfiltered survey**: the seeded 100 across all sections without the size
  filter — a number for heavy packages, no asterisk.
- **More than one device**: a second phone / Android version, and the Google
  Play build of Termux.
- **Upgrade and remove**: `apt upgrade` across a Debian point release; an
  uninstall; Termux untouched afterwards.
- **Clear refusals**: an out-of-scope package says why ("needs a system
  service", "needs root") instead of a raw dpkg error.
- **Demo**: `apt install gcc`, compile, run — all inside Termux.
- **Auto-adopt downloaded glibc programs** (to consider): adopt on first run
  so a `curl ... | bash` installer lands with no manual `dn-adopt`. Adopt in
  place (keeps `/proc/self/exe`), announce on stderr, opt out
  `DN_NO_AUTO_ADOPT=1`.

## Branch `dn-interp-surgical`: the loader-centric runtime

Design: [`docs/spec/dn-interp-shim.md`](docs/spec/dn-interp-shim.md). A modification
of `main`: `dn-interp` is the program's interpreter, `dn-shim.so` (linked with
`dn-policy`) is the in-process fast path and owns `P_GATE`, and `dn-trace` is
the fallback. `dn-policy` and `dn-trace` are reused unchanged; glibc is not
wired. The shim's blind spots (NSS, gconv, loader internals, inline `svc`,
static) fall to the tracer automatically through the filter's gate-IP rule, so
there is no per-binary routing — see
[`docs/reference/syscall-boundary.md`](docs/reference/syscall-boundary.md).

**Build / run split** (the toolchain):

- **Build on WSL (amd64).** The `aarch64-linux-gnu` cross toolchain (gcc 14.2,
  `binutils`, `libc6-dev-arm64-cross`, static `/usr/aarch64-linux-gnu/lib/libc.a`)
  builds `dn-interp`, `dn-trace` (vendored `talloc`) and `dn-shim.so`. Native
  amd64 runs `tools/check-*.py`. Good for compile-verify and static/ELF checks;
  it cannot **run** the aarch64 runtime.
- **Run on the device over `adb`**, inside the debuggable `dn-shell` app
  (`run-as`), which supplies the app-uid / SELinux environment. Not under
  `qemu-user`: its ptrace/seccomp emulation is not faithful for `dn-trace`.

**Test harness** (verified 2026-10-12): device SM-G970W (`beyond0q`), Android 12
/ SDK 31, arm64-v8a. App `org.dn.shell` is debuggable; `run-as org.dn.shell`
gives uid `u0_a291`, context `u:r:runas_app:s0` (no seccomp filter —
`Seccomp: 0`, so this route does not exercise Gate A). Prefixes live under
`files/prefixes/`: `core-deb` (the runtime, entry
`usr/lib/deb-native/dn-monitor -- /usr/bin/bash …/init.sh`) and `dn-gcc` (an
on-device aarch64 toolchain, `aarch64-linux-gnu-gcc-14`). The loop: cross-build
on WSL → `adb push` to `/data/local/tmp` → `run-as org.dn.shell sh …`. A boot
through `core-deb`'s `dn-trace` reports `uname=aarch64` and `id` = `root`
(fake root works).

**Device unblocked (2026-10-12).** The `dn-shell` app shipped `targetSdk=34`,
so Android's W^X (no `execve` from a writable data dir) stopped a prefix
launched through the app. The source (`/root/dn-shell-src`) already set
`targetSdk=28`; rebuilt it on WSL (`build.sh`, signed with
`~/.android/dn-shell.keystore` — the same key as the installed app, so
`adb install -r` kept the prefixes) and reinstalled: `targetSdk=28`,
`core-deb`/`dn-gcc` intact, `run-as` boot still works, storage permissions
re-granted. WSL cross-build stays the working half for the runtime; the device
is now a usable host too.

**Phases** (each leaves the tree runnable):

- **P1 — loader plumbing (no behaviour change).** Recover
  `native/dn-interp.c`/`dn-interp.conf` from `d18b187`; add the build step; derive
  `DN-INTERP` in `cli/dn-trace.c`; exec-gate rule 3 targets `dn-interp`. Verify:
  `.deb` bytes unchanged, `/proc/self/exe` correct, a dynamic program runs.
- **P2 — shim + gate page.** Rewrite the old `path-redirect.c` as
  `dn-shim.c` (call `dn-policy`, issue syscalls from `P_GATE`); remove the gate
  page from `elf/rtld.c`; drop `dn-policy-glibc-wiring.patch` from the
  pipeline; recompute `src/syscalls.tsv`'s `glibc`/`gate` columns.
- **P3 — glibc needs no patch.** Done: the failure the Android patch existed
  for is a seccomp `SIGSYS` on `rseq`/`clone3` at the syscall boundary, answered
  by the tracer's emulation — not a loader-startup fault. Stock Debian
  `libc6` starts and runs under the tracer
  (`docs/reference/android-platform.md`, "Stock `libc6` re-tested"). The patch,
  `scripts/glibc/` and `build-glibc.yml` are retired; the tree takes stock
  `libc6`/`libc-bin` from the mirror, and `build-trace.sh` links `dn-trace`
  static against stock `libc6-dev` with `GLIBC_TUNABLES=glibc.pthread.rseq=0`
  for its own startup.
- **P4 — measure.** Reuse the per-tier counters; per-tier call distribution.

**Status**: P1 done — `dn-interp` recovered (`native/`, `build-interp.sh`) and the
exec gate's rule 3 runs it; the `dn-glibc` → `dn-shim` naming and mechanism
sweep is in.  P2a + P2b-2 done — `dn-shim.so` (`native/dn-shim.c`,
`build-shim.sh`) covers the whole interposed surface: open, stat, access,
the fake-root writes and identity, path manipulation, hardlinks, xattr,
reverse translation and `syscall(2)`, each issuing from `P_GATE`, mirroring
`patches/dn-policy-glibc-wiring.patch`; `dn-interp.conf` preloads it.
P2b-3 next: move `P_GATE` into `dn-interp`, drop `dn-policy-glibc-wiring.patch`
from the pipeline, recompute the catalog's `glibc`/`gate` columns.

## Prefix artifacts: core-ultra / core-deb, one ship path

Design: [`docs/spec/prefix.md`](docs/spec/prefix.md),
[`docs/spec/prefix.md`](docs/spec/prefix.md).

**Status**: shipped. `build-runtime.yml` builds the runtime (`dn-trace`, static
against stock `libc6-dev`); `build-prefix.yml` builds `core-ultra` and
`core-deb` from the mirror's own stock `libc6`/`libc-bin` (an
empty base + pinned `scripts/build/packages.tsv` + `profile.txt`) and publishes
both to the single rolling `prefix` release. On device `ship-prefix.sh` reads
`boot/version`, extracts, runs `boot/install.sh`; the host boots through
`boot/boot.sh`, and the prefix's init completes core-deb on its first boot
(76 packages; `apt install figlet` runs).

**Open**:

- Build `core-ultra` from its own recipe instead of cutting it from the built
  core-deb tree; `core-deb` = that recipe + the Debian layer.
- **core-deb profile gaps**: add `tzdata`, `locales`, `util-linux`, `procps`
  (and `libtalloc2`, already a known issue) to `profile.txt`/`packages.tsv`
  so stock Debian tools plus timezone/i18n work
  ([`known-issues.md`](docs/reference/known-issues.md), "Gaps toward a
  complete Debian").
- ~~**Ship the glibc dev set at `+dnN`**~~ **moot on this branch**: `libc6` is
  Debian's own stock package now, so `libc6-dev`'s `Depends: libc6 (= <base>)`
  is already satisfied and `apt install gcc` needs no repackage.
- **Bootstrap on a poor host**: where the first artifact comes from, and how
  one artifact yields the next (core-ultra, core-deb, specialized).
- The `claude` specialized prefix: core-ultra recipe + the Claude binary as a
  glibc ELF at build time.
- `core-ultra` size: most `gconv` modules; terminfo missing.
- Launcher symlinks: make them relative after `normalize_symlinks` runs (the
  post hook's order leaves them absolute).
- **Package list refresh**: `scripts/build/packages.tsv` pins exact versions;
  a mirror point release makes them unreachable. A resolver, or a documented
  refresh step, is needed.
- **One build entry**: wrap the build steps in a single command (a `Makefile`
  or `scripts/build.sh`), so the README's build section is one line.

## Runtime v1: new runtime (dn-policy / dn-shim / dn-trace)

Design: [`docs/spec/runtime.md`](docs/spec/runtime.md). Replaces the whole
current runtime (`dn-shim.c`, `dn-run`'s adopt-on-first-run, `dn-elf` at run
time, `patch-maintainer-scripts.sh`) rather than running beside it. Phases
below match `runtime.md`'s roadmap; each phase must leave the tree runnable
on the current runtime until it's actually replaced.

**Status**: P1 mostly done, reusing `src/tracer/` (the pruned PRoot fork) as
`dn-trace`'s core rather than writing one fresh — answers the open question
below.

**P1 — exec gate, observe-only**:

- [x] `dn-trace` as the tree's root process needs no new boot-sequence code:
  `cli/dn-trace.c`'s existing `dn-trace [-v LEVEL] [-b HOST[:GUEST]]... --
  PROGRAM [ARG...]` already takes any script as `PROGRAM` (`dn-trace --
  any-init.sh`), and `launch_process()` (`tracee/event.c:139`) already
  installs the seccomp filter exactly once, which the kernel then carries
  across every `fork`/`exec` in the tree on its own — no "adopt per static
  binary" step needed once `dn-trace` is what starts the tree.
- [x] Exec gate classifies every `execve`/`execveat` by ELF header (the 5
  rules in `runtime.md`): `classify_exec()` in `execve/enter.c`, logged via
  `VERBOSE` only, changes nothing else yet. Verified by hand: a script, a
  static binary, a dynamic glibc binary, a `PT_INTERP` repointed to a
  foreign loader, and a plain non-ELF file each land in the rule their
  header says they should.
- **Not strictly minimal**: the filter installed today (`proot_sysnums` +
  `fakeroot_sysnums` in `monitor/monitor.c`) already traces the path and
  identity groups too, not just `execve`/`execveat` — wider than P1's "exec
  rule only." Left as is rather than adding a toggle to shrink it, since
  nothing depends on it being minimal and it'll be superseded for real once
  dn-policy lands in P2/P3.
- Done when: `init.sh` and the existing services run under this `dn-trace`
  with no new errors, and a day's real use has a classification log.
  **Remaining**: the "real tree, real day" run — so far only verified by
  hand against one-off commands, not a booted prefix.

**P2 — the fast path**:

- [x] Define dn-policy's calling convention before either caller is written:
  `src/dn-policy/dn-policy.h` — function signatures, 0/-errno returns,
  caller-supplied buffers only (no library heap crossing the glibc/talloc
  allocator boundary, except the opaque `DnIdentity` handle, matched
  alloc/free), thread safety, and why `process_vm_readv`/`writev`
  marshalling is `dn-trace`'s own wrapper's job, not dn-policy's. Caught
  one real bug while writing it: fake uid/gid can't be a dn-policy global,
  since `dn-trace` tracks many tracees at once (one may drop to `_apt`
  while another stays root) — fixed with a `DnIdentity` handle the caller
  owns one of per traced program.
- [x] Implement path mapping: `src/dn-policy/dn-policy.c` —
  `dn_policy_init()`, longest-prefix mapping, `/proc`/`/sys`/`/dev`
  passthrough, no-double-translation, reverse translation. Verified by
  hand (`.check_translate.c`), caught and fixed a real bug (a spurious
  trailing slash translating guest `/`). **Not yet implemented:**
  absolute in-tree symlink resolution (correct for a tree with none,
  see the file's header comment).
- [x] Fake root: `src/dn-policy/dn-policy-fakeroot.c` — owner store
  (probed on-device: `user.dn.*` xattr works on the app's own data
  partition, so that's the default backend; the `RT/state/` DB fallback
  is implemented and reachable via `DN_POLICY_OWNER_BACKEND=db` for a
  device where it doesn't), `security.*` xattr faking, the
  `DnIdentity`-keyed get/setuid. Verified by hand against both backends
  (`.check_fakeroot.c`), including a slot-reuse case for the DB backend.
- [x] Hardlinks: `src/dn-policy/dn-policy-hardlink.c` — link2symlink, a
  refcount DB, and the stat/lstat fixup that makes a managed name report
  as an ordinary multiply-linked regular file. GPL question resolved (not
  actually a blocker, see `runtime.md`). Verified by hand
  (`.check_hardlink.c`).
- [x] Absolute in-tree symlink resolution: `map_and_resolve()`/
  `resolve_symlink_chain()` in `dn-policy.c`, with a `normalize_into()`
  cleanup pass for a relative symlink's `..`. Verified by hand
  (`.check_symlink.c`): absolute/relative/chained symlinks, a component
  appended past one, `_nofollow`, a not-yet-existing target, and
  `-ELOOP` on a self-reference. **dn-policy is now feature-complete**
  for everything `dn-policy.h` declares; not yet done: caching the
  resolution (every lookup hits the real filesystem directly).
- Patch glibc (full rebuild from Debian source, not the shipped 10-file
  swap — see `docs/spec/runtime.md`'s status note): wire dn-policy
  into every path-taking function (public + internal + `syscall()`), route
  every kernel call through the gate page, map the gate page at loader
  startup, reorder library search (`RT/lib` first).
  - Slices done in `patches/dn-policy-glibc-wiring.patch`: the public
    `open`/`openat` family, the stat/access family (`fstatat64.c`,
    `statx.c`, `faccessat.c`), path translation for the simple manipulation
    wrappers (`mkdir`, `rmdir`, `rename`/`renameat`/`renameat2`, `symlink`,
    `truncate`, `utimensat`/`utimes`/`utime`, `statfs`), reverse translation
    for `getcwd()` and `readlink()` of the `/proc/self` magic links (with
    `/proc/self/exe` answering the real program path under a rule-3 launch),
    the `syscall(2)` interposition for the path group, fake root's writes
    (`chown`/`lchown`/`chmod`/`fchmodat` via `dn_policy_owner_merge()`),
    hardlinks (`link`/`unlink` via `dn_policy_link()`/`dn_policy_unlink()`),
    the loader's mapping of the fixed gate page (`P_GATE`), the `RT/lib`-first
    library search order, and
    `dn_policy_fake_stat()` (owner store) on a successful `stat`. rtld is
    kept out entirely (`#if !IS_IN (rtld)`, plus `dn_policy_fake_stat` in
    `elf/Makefile`'s `rtld-stubbed-symbols`). A full `make -O -j8` of glibc
    `2.41-12+deb13u4` with it applied links clean, `ld.so` ends up with no
    dn-policy symbol, and the minimal prefix run under the built loader
    shows `open`/`stat`/`lstat`, the fake-root post-processing, the reverse
    translation and the manipulation wrappers all working (verified
    2026-10-08; see `patches/README.md`). Not applied by the build on branch
    `dn-interp-surgical`: the fast path is the shim, and the pipeline applies
    no glibc patch at all.
    - **Known issue (accepted):** the `syscalls.list`-generated wrappers
      (`mkdirat`/`unlinkat`/`symlinkat`/`linkat`/`readlinkat`/`fchownat`/
      `chdir`/`chroot`/`*xattr`) and any path-group call a static/foreign
      program makes directly fall to the `ptrace` tier — correct, just
      slower. The gate page is mapped but inert until routing every glibc
      syscall through it, which must come together with overriding those
      wrappers and handling `syscall_cancel.S`'s cancellation markers.
- Pick `P_GATE`, done: `0x100000000` (4 GiB) — found free in every readable
  on-device process map, inside the 39-bit range; the loader maps it
  (`elf/rtld.c`), `r-xp`, and libc issues its syscalls from it
  (`INTERNAL_SYSCALL_RAW` + `syscallS.S`). The filter's gate-IP rule is now
  built from the catalog (`src/syscalls.tsv`, embedded in `dn-trace` at
  build time): only the syscalls every libc entry point already translates
  in-process (`gate=yes`) are exempted. Override the `syscalls.list`
  wrappers (`mkdirat`/`unlinkat`/`symlinkat`/`readlinkat`) to flip theirs. Still open: wire `bind`, the identity group
  (fake root), and the cancelable `syscall_cancel.S` path, then flip their
  catalog rows to `gate=yes` — the list stays the one source of truth.
- Turn on exec-gate rules 3/4 (rewrite to `RT/ld.so ...`). Drop the shim.
  - Rule 3 done: `dn-trace TREE LOADER` rewrites a glibc-dynamic exec to
    `LOADER --argv0 <orig argv0> <real path> <args>`. Rule 4 (a foreign
    loader) still to do.
  Switch glibc-family packages to version pinning (`/etc/apt/preferences.d/dn-shim`,
  auto-generated from `Source: glibc`), drop the hand-written resolver.
- Done when: packages in use, reinstalled from stock `.deb`s, run correctly;
  `apt update`/`install`/`upgrade` run end to end with no install-script
  failures.

**P3 — the fallback path**: turn on the path/identity syscall groups in the
shared filter; `dn-trace` handles them via `ptrace` through dn-policy,
replacing the current ptrace mechanism. **Must actually remove**
`src/tracer/syscall/exit.c`'s own `dn_fake_root()` (and the
`fakeroot_sysnums` list in `monitor/monitor.c`) once dn-policy's fake root
is wired in — not leave both running side by side indefinitely, or
principle 3 ("one policy, two enforcement points, so they can never
disagree") is violated by the very code meant to honor it. Done when
`busybox-static`, a static Go program, and a Go-with-cgo program all run
correctly.

**P4 — measure and optimize**: per-program/per-syscall counters in
`dn-trace`; `process_vm_readv`/`writev`; path-resolution and exec-gate
classification caches. Done when there's a per-tier call-distribution table
for installing packages, compiling, and running a Python script.

**P5 — optional**: seccomp user notification for hot calls, only if P4's
numbers justify it.

## Backend emulation (faking what the kernel refuses)

Design: [`docs/spec/emulation.md`](docs/spec/emulation.md). Reimplement the
operations Android refuses (mount/runtime, raw sockets, privileged ports,
capabilities, `/proc` reads) in userspace, and answer every interface that
reads them from one model in dn-policy. The existing
`mount`/`unshare` binding table, AF_NETLINK substitution and
`statfs`/`/dev/shm` fake are the template (principle: *answer in place of the
kernel*, and cover every reader so the two enforcement points cannot disagree).

**Open**:

- **Virtual mount table, complete**: `statfs`/`statvfs`/`statx` and
  `/proc/self/mounts`/`mountinfo` all read the table `apply_emulated_mount`
  already writes; today only the binding is recorded.
- **Overlay copy-up**: userspace CoW for a write on a lower layer, whiteouts
  on delete, and a merged `readdir` — no overlayfs on this kernel.
- **Synthetic `/proc`/`/sys`**: a `path -> generator` table covering
  `/proc/sys/*`, `/proc/net/*`, `/proc/version`, `/proc/self/*`, served on
  `open`/`stat`/`read`.
- **Privileged ports**: a bind-translation table plus `getsockname`/`accept`/
  `connect` and `/proc/net/tcp` back-translation.
- **Raw ICMP**: substitute the unprivileged ping socket (verify
  `ping_group_range`), else a userspace helper.
- **Capabilities**: `capget`/`PR_CAPBSET_READ`/`capset` synthesis.
- **Normalize divergent platform behaviour**: the silent class (`uname`,
  `/proc` values, `rlimit`/cgroup, W^X `mprotect`) reports Android and raises
  no error — make the observation match what a package expects, not just
  answer the call. Do it in dn-policy and apply it at **both** tiers
  (dn-shim for the fast path, dn-trace for static/raw syscalls), not in
  dn-trace alone ([`emulation.md`](docs/spec/emulation.md), "Two kinds of
  hard limit").
- **Refuse, don't fake**: real isolation, TUN and LSM-observable state get a
  clear refusal, not a fiction.

## FHS-to-Android mapping contract

**Status**: survey complete on `dn-stock` (SM-G970W, Android 12 / SDK 31).
Implementation is not started. This is the shared filesystem contract for the
prefix, not a new `dn-view` runtime component. The behavior is implemented by
the existing `dn-shim`, `dn-trace` and `dn-emu` paths.

The first survey classified the FHS surface into four outcomes:

- **Direct overlay** — the guest path maps to a real path under `TREE` or to a
  host path through a binding. Common FHS paths (`/usr`, `/etc`, `/var`,
  `/run`, `/tmp`, `/home`, `/root`, `/opt`, `/srv`, `/media`) are in this class.
- **Indirect overlay** — the path has a backing or answer, but Android does not
  provide Debian semantics directly. Examples: `/dev/shm`, `/proc/version`,
  `/proc/sys/*`, selected `/sys` values, and the resolver/runtime files.
- **Emulation** — the kernel or Android policy refuses the operation, but a
  coherent userspace model can answer it. The model must cover all readers:
  `open`, `stat`, `statvfs`, `read`, `readdir`, `readlink` and reverse paths.
- **Refuse** — isolation, LSM-observable security state, TUN, real
  capabilities, privileged ports, raw sockets or other operations that cannot
  be represented safely. Return a clear error and reason; never claim success.

### Implementation plan

1. **Freeze the inventory.** Maintain a table of guest path, Android backing,
   operation classes, lifecycle, security boundary and classification. Cover
   `/usr`, `/etc`, `/var`, `/run`, `/tmp`, `/home`, `/root`, `/mnt`, `/media`,
   `/dev`, `/proc`, `/sys`, `/opt` and `/srv`.

### Blocker: one root model, one shared path translator

**Found by on-device probe** (`dn-stock`, SM-G970W). The direct overlay work
cannot be correct until this is settled. A dynamic program and a static one see
two different filesystem roots in the same boot:

```text
dynamic (shim lane):  readlink /proc/self/root = /
static  (tracer lane): readlink /proc/self/root = TREE
```

Probing the same binding `/sdcard -> /mnt`:

| lane | `/etc/passwd` | `/mnt/...` | `/sdcard/...` |
| --- | --- | --- | --- |
| dynamic (shim) | ok | **ENOENT** | **ENOENT** |
| static (tracer) | ok | ok | **ENOENT** |

Root cause: **two path models that disagree**, with no shared definition of
"guest path" versus "host path".

- The shim lane runs with the real kernel root `/`; `dn-shim` rewrites a guest
  path to an absolute host path (`/etc/passwd` -> `TREE/etc/passwd`,
  `/mnt/...` -> `/sdcard/...`) and issues it from the gate page.
- The tracer lane presents the tree as the guest root (PRoot root binding);
  its own `translate_path()` maps guest -> host with PRoot bindings, separate
  from `dn-policy`.
- A single call can be gate-ALLOWed (kernel sees `/`) or TRACEd (PRoot sees
  `TREE`) depending on the filter, so one process has two path spaces at once.
- When a shim-produced host path (`/sdcard/...`) is still TRACEd, PRoot treats
  it as a guest path and maps it again (`TREE/sdcard/...`) — double translation.

Neither `dn_policy_translate_path()` (used by the shim) nor PRoot's
`translate_path()` (used by the tracer) is the single source of truth, and the
"already-translated / already-host" predicate is not shared. Small guards in
either place do not fix the class of bug.

**Required mechanism, before direct/indirect overlay (Android-allowed scope
only; `mount`/`chroot`/namespaces are Android-denied and out of scope):**

1. **Programs see TREE as root.** This is semantic, produced by translation,
   not a kernel `chroot` (which Android denies).
2. **One overlay table in `dn-policy`**, one row per mapping:
   `<android_absolute> > <tree_absolute>` (equivalently host > guest). The tree
   root itself is the default: a guest path with no matching row maps to
   `TREE + guest`. Longest tree-side prefix wins.
3. **One translator.** `dn-shim` and `dn-trace` use the same mapping law; the
   tracer stops translating the deb-native root with PRoot's private tables.
4. **One "already host" predicate.** A path already under `TREE` or under a
   row's android side is host-shaped and returned unchanged, in both lanes.
5. **Coverage.** Every path-bearing syscall must be translated by the lane that
   handles it (shim in-process, tracer otherwise), so a gate-ALLOWed call and a
   TRACEd call observe the same host path. This is a coverage rule, not a
   requirement that both call one physical function.
6. **The table is a path mechanism only.** A row answers one question: which
   android path is bound at this tree path (`android > tree`). Nothing about
   syscalls belongs here. Whether the kernel then serves a path, refuses it
   (SELinux/seccomp), or behaves differently is a separate layer handled by
   `dn-monitor`/`dn-emu`, not by this table.

**Done when:** the same guest path resolves to the same host resource in both
lanes — dynamic, static/raw, and via a direct mapping like `/mnt/...` — with a
`stat`/`open`/`read`/`readdir`/`readlink` matrix that agrees.

**Progress / findings (on-device, 2026-10-12, `dn-stock`):**

- Direct overlay `/mnt` now works in both lanes: write+read,
  `lstat`/`stat`/`open`/`readdir` agree for dynamic (shim) and static
  (tracer). `/sdcard` is not exposed as a guest path (correct).
- A row's android side must be **canonical** (e.g. `/storage/emulated/0`, not
  `/sdcard`): PRoot canonicalizes binding hosts, so a non-canonical row makes
  the two lanes disagree. `dn-shim` cannot canonicalize itself — calling
  `realpath()` inside the shim recurses through its own interposed
  `stat`/`readlink` and fails. So the table must carry canonical android paths
  (written by the host, or canonicalized by `dn-monitor` and shared), or
  `dn-policy` must canonicalize with raw syscalls.
- Performance guard: `HAVE_SECCOMP_FILTER` must be defined in the runtime
  build. Adding `-Wl,--wrap=__libc_start_main` to the global `LDFLAGS` broke
  the `.check_*` feature probes, silently dropped the filter, and left every
  syscall on `ptrace` (~8x slower: 100k `getpid` 0.45s -> 17s; `open` 4.6s ->
  12.6s). The wrap must apply only to the `dn-monitor` link.
- Remaining: make `dn-trace` use the `dn-policy` translator (not PRoot's
  private tables). Indirect resources (`/dev/shm`, synthetic `/proc`) are
  `dn-emu` work, done later; they are not rows in this table.
- **Android survey (app sandbox):** the app can reach `/dev`, `/proc`, `/sys`
  (with limits), `/sdcard` = `/storage/emulated/0`, `/mnt` (partly) and its own
  `/data/user/0/<pkg>`. It cannot list `/`, `/data` or `/storage` top level. So
  passthrough/external can only cover the kernel pseudo-fs and storage; the
  Debian userland (`/usr`, `/etc`, `/var`, `/home`, `/root`, `/run`, `/tmp`,
  `/opt`, `/srv`, the `/bin`,`/sbin`,`/lib` links) has no Android source and
  must stay in the tree. Only mount-point-only dirs become rows.
- **Rows done:** `/mnt` and `/media` -> `/storage/emulated/0`, `/dev/shm` -> a
  real directory under the app's `files/`.

### Remaining work (plan)

The FHS surface still open splits into two mechanisms; the ordering is by how
many programs break without them.

**A. Missing backing — path rows (direct).**

- `/dev/shm` — POSIX shared memory; DBs, Python multiprocessing, browsers.
- `/dev/pts` — ptys; terminal, `script`, `expect`, ssh sessions.
- `/dev/tty`, `/dev/full` — a few tools.
- `/media` — removable storage (same shape as `/mnt`).

If a real app-data directory can back each one, each stays a row and both lanes
consume it through the table.

Survey (2026-10-12, `dn-stock`):

- Device `/dev`: `null`, `zero`, `random`, `urandom`, `ptmx` exist (mode 666,
  open ok); `tty` exists but open is `ENXIO` (no controlling terminal);
  `full` is absent/denied; `shm` is absent.
- No tmpfs for `shm` anywhere. App data lives on `/dev/block/sda31` (real fs,
  ~227G, writable), so a real directory under `files/` backs `/dev/shm`.
  It would not be tmpfs, so `statvfs` reports the data partition, not a small
  tmpfs — acceptable for basic POSIX shm, revisit if a program depends on tmpfs
  semantics.
- PTY: **works.** `/dev/ptmx` is passed through and the full glibc flow
  (`posix_openpt` -> `grantpt` -> `unlockpt` -> `ptsname` -> open slave ->
  write/read) succeeds in the prefix, both lanes. An earlier "fails" reading
  was a probe bug (it closed the fd before `grantpt`). No `/dev/pts` row is
  needed: it is passthrough, already present.
- `/dev/tty`: open is `ENXIO` with no controlling terminal, which is the same
  as on a real Debian shell until a program allocates a pty and sets it as the
  controlling terminal (`setsid` + `TIOCSCTTY`). Not a defect.
- `/dev/full`: absent; a program that writes to it for an `ENOSPC` test would
  fail. Minor; a future `dn-emu` answer (read -> zeros, write -> `ENOSPC`) if
  needed.
- `/media`: `/storage` top level is denied; `/storage/emulated/0` is reachable,
  so `/media` maps to the same backing as `/mnt`.

**Done in A:** `/media` -> `/storage/emulated/0`, and `/dev/shm` -> a real
directory under the app's `files/`. Verified on both lanes: `/dev/shm`
write/read/`readdir`, `shm_open`/`mmap`, and `/media` all work; `/dev/null`
still passthrough. The `already host` predicate now uses the **longest** host
vs guest match, so `/dev/shm` is not mistaken for the `/dev` passthrough host
side.

**Still open in A:** `/dev/full` (minor; `dn-emu` later). `/dev/pts` needed no
work (passthrough already works), `/dev/tty` needs a controlling terminal that
only a program's own `setsid`+`TIOCSCTTY` provides.

**B. Kernel/Android-refused — emulation (`dn-emu`).**

- `/proc/version`, `/proc/sys/*` — read by `configure` and build scripts.
- `/sys/*` — hardware/CPU info.
- `/proc/mounts`, `/proc/self/mountinfo` — must reflect the overlay view, not
  Android's mounts.

A refusal cannot be fixed by a path row; the answer must be synthesized.

**C. Beyond FHS, needed for deeper integration.**

- NSS/accounts: `/etc/passwd`, `/etc/group`, `getpwnam` consistent with fake
  root.
- Network: `/proc/net`, DNS, sockets.
- Services: `/run`, `/var/lock`, runit.
- Host bridge (the `dn-shell` app side): storage, notifications, DNS.

Order: A first (`/media`, then `/dev/shm`, `/dev/pts`), then B, then C.

### Direct overlay plan

1. **Define one overlay record.** One row: `<android_absolute> > <tree_absolute>`
   (host > guest; equivalently the earlier `host<TAB>guest<TAB>flags` shape).
   The tree side is the lookup key; longest tree prefix wins. Keep symmetric
   rows explicit (`/proc`, `/sys`, `/dev`) and allow asymmetric android
   resources (`/sdcard` or `app/files/mnt` > `/mnt`).
2. **Centralize the table in `dn-policy`.** Add init-time binding loading and
   lookup there. Remove the duplicated hardcoded passthrough list from the
   policy and the monitor. The same table must serve guest-to-host translation,
   host-to-guest reverse translation and symlink decisions.
3. **Choose a host-provided input.** Store bindings in a prefix-owned file
   under `var/lib/deb-native/` or `RT/state/`, written before boot. `boot.sh`
   must not need to know individual bindings; `dn-monitor` loads the table
   before it forks the traced init. The file is data, not executable code.
4. **Apply bindings to both executors.** `dn-monitor` converts each record into
   its existing tracer binding table. `dn-shim` uses the same records for its
   cheap direct redirect. A dynamic program and a static/raw program must see
   the same `/mnt`, `/proc`, `/sys` and `/dev` resource.
5. **Make `/mnt` the first real binding.** The `dn-shell` host writes one
   binding for its app-owned `files/mnt` before boot. The existing storage links
   (`Download`, `Pictures`, and so on) remain host-side data; the prefix sees
   them through `/mnt` rather than through escaping relative symlinks.
6. **Define mutation rules.** Bindings are fixed for the lifetime of a boot.
   A changed table requires a new boot, so both executors cannot observe
   different tables. Reject malformed, duplicate or escaping guest paths
   before the monitor starts.
7. **Acceptance tests.** Test read, write, `stat`, `readdir`, `readlink` and
   `getcwd` for `/mnt/Download` through a dynamic glibc program and a static
   or tracer-routed program. Test `/proc`, `/sys` and `/dev` regression too.

### Indirect overlay plan

1. **Define a model contract.** Each indirect path gets a model describing
   `open`, `stat`, `statvfs`, `access`, `read`, `readdir`, `readlink`, create,
   delete and reverse translation. Do not fix one reader while leaving another
   inconsistent.
2. **Implement `/dev/shm` first.** Give it one backing directory under the
   runtime state or a dedicated tree tmp area. Make `shm_open`/`sem_open`,
   direct `/dev/shm/<name>`, `stat`, `access`, `readdir` and `statvfs` agree on
   existence and filesystem identity. Keep the model shared by shim and
   tracer.
3. **Add the virtual mount table.** Record direct bindings and indirect
   resources in one table. Use it for `statfs`/`statvfs` and the relevant
   `/proc/self/mounts`/`mountinfo` reads instead of returning unrelated Android
   state.
4. **Add synthetic global `/proc` files.** Start with `/proc/version` and
   `/proc/sys/kernel/*` only where a Debian program needs a stable answer.
   Serve the same result for `open`, `stat`, `read` and `access`; preserve
   `/proc/self/*` passthrough behavior.
5. **Handle `/sys` conservatively.** Keep readable, supported values as
   passthrough or explicit synthetic answers. Return a clear refusal for
   writes or security-sensitive state Android denies; do not fabricate
   capabilities or kernel controls.
6. **Route the model consistently.** Dynamic calls enter through `dn-shim`,
   libc-internal/raw/static calls enter through `dn-trace`, and refused
   operations reach `dn-emu`. The model and result must be the same at every
   entry point.
7. **Acceptance tests.** For each model run a dynamic test, a static/raw test,
   direct path probes, metadata probes and a real Debian consumer (`apt`, a
   configure script, or a package post-install check). Record unsupported
   operations as intentional refusals.

### Shared follow-up

1. **Test both enforcement points.** Every mapping/model gets a dynamic glibc
   test through `dn-shim`, a static/raw or tracer test through `dn-trace`, and
   an Android-host probe run outside the prefix. A feature is not complete if
   only one executor sees it.
2. **Keep the fast path cheap.** Direct bindings remain string substitution;
   filesystem inspection and model generation must not be added to ordinary
   path calls.
3. **Measure before and after.** Repeat the path benchmarks, package install,
   shell startup and storage access after each table/model change.

### First acceptance ladder

- `/mnt/Download` reads the same storage through shim and tracer.
- `/dev/shm` has consistent `open`, `stat`, `statvfs`, `readdir` and
  `shm_open` behavior.
- `/proc/self/*` continues to work while global `/proc` refusals become either
  synthetic answers or documented errors.
- `/sys` exposes only explicitly supported reads; denied writes remain clear
  refusals.
- `apt`, `dpkg`, shell startup and the current path fast-path benchmarks show
  no regression.

## Per-userland home (planned)

Give each prefix a **sparse `$HOME`** (`/data/data/com.termux/files/.dn/<name>/`)
for program state, while `/home` stays the user's data home, joined by **leaf**
symlinks — ends the two worlds' dotfile/config collisions without recursion.

## Services, then sudo (after alpha)

**Goal**: same scope as `sudo-less` — a service needs something to run it and
the rights it expects. In order: services, sudo modes, then both.

**Status**: not wired into the prefix. `sudo`/`doas` are pinned out; the no-op
privilege layer (`$DN/usr/lib/deb-native/priv/`) is a later real mode's
replacement. Design: supervisor is **runit**, deployed as the programs
unpacked from Debian's `runit` `.deb` (the package itself pulls `adduser`,
`passwd` and PAM through `sysuser-helper`, and its init glue is skipped),
`runsvdir "$DN/etc/service"` started by us. Checked on a device: `runsvdir`,
`runsv`, `sv` and `chpst` run adopted, supervise a service, restart one that
exits, and take `sv down`/`sv up`. Packages keep shipping systemd units; we translate the unit at
install (foreground `ExecStart`, `Environment`, `WorkingDirectory`,
`RuntimeDirectory`/`StateDirectory`; `User=` and sandbox options dropped; low
ports/capabilities/devices refused with the reason) and the `systemctl` front
is a vendored, pruned SINS. Test ladder: `cron` -> `redis` -> `dbus`.

**Open**:

- **Deploy runit**: put the unpacked programs on the `PATH` of whatever starts
  `runsvdir` (it starts `runsv` by name; off `PATH` it logs `unable to start
  runsv`). Not yet checked: `svlogd` logging, `chpst -u`, `runit-init` and the
  `/etc/runit/{1,2,3}` stages, `runsvdir` surviving the end of the session
  that started it.
- **SSH service**: `dropbear` under `runsvdir` (OpenSSH's `sshd` cannot run:
  its privilege-separation `chroot` is blocked). `dropbear` logs in with a key
  today when started by hand; it needs a valid login shell in `/etc/passwd`
  and `/etc/shells` (see the account files item above), and `libtalloc2`.
- `update-rc.d` / `invoke-rc.d` / `deb-systemd-helper` -> real runit
  translation (needs `/run` in the shim, system users in the prefix's db).
- Keep `dn-run`'s preload handling general enough for a second `LD_PRELOAD`
  (a fake-root library beside the shim).
- Keep `base-passwd`'s `root` user and `sudo` group as Debian has them.

## Freestanding tracer (`dn-tiny` prototype)

A 1,800-line tracer with no libc (`src/dn-tiny/`, built by `scripts/build/build-tiny.sh`) bootstraps a prefix on the Android 12 phone and runs
`python3`, `git`, `curl`, `gcc`, `make` in the app domain; what it does and what it
found is in [`docs/reference/ptrace-interception.md`](docs/reference/ptrace-interception.md).
The decision to make is whether it replaces `dn-monitor`'s path layer; these are the
checks and the work still open.

**Checks not done yet**:

- **Android 16 phone**: the same bootstrap and the tests after it. Hard links and
  `openat2` / `faccessat2` / `clone3` behave differently there (the census), and the
  prefix has to be pushed to the phone (127 MB).
- **Guest isolation (L1)**: `/proc/self/root` still reaches the host's token and
  socket, and two environment variables name the host's path. Decide whether the
  prototype must close this before it counts as ready (`monitor.md`, "Where the model
  leaks"; `fhs-projection.md`).
- **A controlled speed comparison**: the same bootstrap under `dn-monitor` and
  `dn-tiny`, same prefix, same phone, repeated. The 196 s against about 5 minutes is
  not one.
- **Another phone** and the Google Play build of the launcher app (as in the alpha
  goal).
- **More packages**: `perl` with an XS module, `nodejs`, `ruby`; `apt upgrade`;
  uninstall; a package whose maintainer script uses a hard link in the way `git clone
  /local/path` does (inode comparison).

**Open work**:

- **pid-indexed tables**: the id table and the `/proc/<pid>/exe` table are
  `pid % 16384`; this phone's `pid_max` is 32768. Size them from `pid_max`.
- **Owner store**: `chown` is accepted and not kept (`user.dn.*` was the plan).
- **`dirfd` resolution**: a deleted directory behind `/proc/<pid>/fd/<n>`;
  `execveat`; `openat2` with `RESOLVE_*` (answers `ENOSYS`).
- **Rows from the catalog**: generate the rows from `src/syscalls.tsv` instead of the
  tables in `gen_rows.py` (see "Coverage of the rows" below).
- **Hard links**: `linkat` is a copy. A real `link2symlink` (`dn-policy-hardlink.c`)
  would keep the two names one file; do it only if a real program needs it.
- **Mount-family calls** (`mount`, `umount2`, `chroot`) by path mapping; the
  `CPU pinning` policy (off by default, probe the cores at run time).
- **Wiring**: the code is in `src/dn-tiny/` (branch `tracer/dn-tiny`) but nothing ships
  it: `boot/boot.sh` starts `dn-monitor`. Decide how it starts (a flag, or replacing
  `dn-monitor`), add it to the artifact build, and update `monitor.md` ("single
  decision point" becomes: the monitor executes, the policy decides) and the credits
  once no PRoot-derived data (the rows) remains.

**Speed** (compiling is the priority workload; the threat model is accidental leaks
only, not a security boundary):

Measured on the Android 12 phone, in Termux, 2026-10-14: `make -j1` of Lua 5.4.7 with
gcc under `dn-tiny` takes about 50 s with 118k stops (67.6k entry, 50.6k exit, 52k
mapped paths, 189 execs); Termux's own clang builds the same tree natively in 9.9 s
(a reference only, another compiler). One stop costs 120-600 us even in `-m bare`
(native `stat` is 0.8 us), so the time is set by the **number of stops**; the path
work adds about 80 us per stop (`full` against `bare`). Stops by syscall:

| syscall | entry | exit | share |
| --- | --- | --- | --- |
| `readlinkat` | 38185 | 38185 | 64.6% |
| `openat` | 10381 | 0 | 8.8% |
| `fstat` | 5183 | 5183 | 8.8% |
| `newfstatat` | 3099 | 3099 | 5.2% |
| `brk` | 3035 | 3035 | 5.1% |
| `close` | 5303 | 0 | 4.5% |

The core pinned with `-c` changed the wall time from 20.5 s (cpu5, one run, not
repeated) to 61 s (cpu4, same cluster): not understood yet, re-measure before relying
on affinity.

Those figures are from the rows PRoot's tables gave (86 rows, 33 with an exit stop).
`gen_rows.py` now catches only the syscalls the tracer acts on, hottest first, and asks
for the exit stop only where a result is rewritten (51 rows, 5 with an exit stop); the
same build then makes 104.9k stops (58.3k entry, 46.6k exit, the same 51.9k mapped
paths) in 46.6 s, and the checks after the bootstrap still pass.

Algorithm work in `dn-tiny` first (it stays the net for static programs and
libc-internal calls whatever the fast path does):

- **`readlinkat` exit stop only when needed**: the walk at entry already knows whether
  the target is a symlink; a non-link answers `EINVAL` and a link inside the prefix
  already holds a guest path, so only `/proc` links (and links to host paths) need the
  result rewritten. About 38k stops on the Lua build.
- **Owner faking on demand**: the `fstat` / `newfstatat` exit stops exist for fake root
  owners and symlink sizes; make owner faking switchable (rule 5 of `native.md`) and
  decide the symlink-size fix at entry. About 8k stops.
- **A resolution cache** in the tracer (component -> link or not, target), invalidated
  by the mutating calls it already traces (`rename*`, `unlink*`, `symlink*`, `mkdir*`,
  `linkat`).
- **Who calls `readlinkat`**: the guess is `realpath()` in `cpp`/`gcc`/libiberty
  (glibc resolves it with an internal `__readlink`, one call per component). Confirm
  with a `realpath` test program and per-program counts.

**Coverage of the rows** (surveyed 2026-10-14 against the arm64 syscalls that take or
return a path, a socket path, an exec or an identity):

- **Mapped and complete**: `openat`, `openat2`, `newfstatat`, `statx`, `faccessat(2)`,
  `readlinkat`, `mkdirat`, `mknodat`, `unlinkat`, `symlinkat`, `linkat`, `renameat(2)`,
  `fchmodat(2)`, `fchownat`, `utimensat`, `truncate`, `statfs`, `chdir`, `chroot`, the
  eight `xattr` calls, `inotify_add_watch`, `execve` (the gate), `getcwd`, the
  identity calls and the owners in `stat` results.
- **Gaps** (not caught, and nothing would be done with them yet; each needs its
  action before it gets a row):
  - **AF_UNIX socket paths**: `bind`, `connect`, `sendto`, `sendmsg` in; `accept(4)`,
    `getsockname`, `getpeername`, `recvfrom`, `recvmsg` out. A guest `bind("/tmp/x.sock")`
    gets `ENOENT` (it reaches the host's `/tmp`): tmux, ssh-agent, gpg-agent, dbus, and
    the host bridge into a prefix, all need it.
  - **`execveat`**: not sent through the exec gate; glibc's `fexecve` uses
    `execveat(fd, "", AT_EMPTY_PATH)` and the kernel then looks for the host's
    `/lib/ld-linux-aarch64.so.1`.
  - **`name_to_handle_at`**: its path argument is not mapped (a real leak, rarely used).
- **No row needed**: `mount`, `umount2`, `swapon`, `swapoff` are trapped by Android's
  filter, which outranks TRACE, and answered at the `SIGSYS` stop whatever the rows say;
  `pivot_root` and `acct` fail with `EPERM` on the capability check before any path is
  looked up.
- **Missing for newer kernels** (ENOSYS on this 4.14 kernel, present on Android 16's):
  `open_tree` (5.2; unprivileged without `OPEN_TREE_CLONE`, an `O_PATH` open), `move_mount`,
  `fspick`, `mount_setattr`, `fanotify_mark` (path in argument 4), `setxattrat`,
  `getxattrat`, `listxattrat`, `removexattrat` (6.13), `file_getattr`, `file_setattr` (6.17).
- The rest is the catalog: one explicit table (`src/syscalls.tsv`) giving each syscall a
  class (path, result, socket, exec, identity, refused by the platform, none) and the
  rows generated from it.

**Fast lane for the shim** (later, if the numbers after the work above still call for
it; replaces the P-gate):

- **Cookie in a spare argument register instead of the gate page.** Every path syscall
  leaves `x5` free; the shim issues the call itself with a 64-bit cookie in `x5`, and
  one test at the top of the filter (about 4 instructions) ALLOWs it. No fixed mapping,
  no trampoline, a shorter filter than the 7 instructions per exempted syscall the gate
  costs. As tight as the gate (any code can set `x5`), enough for accidental leaks.
- **Load the shim through the exec gate**: `dn-tiny` already rewrites the exec to
  `ld.so --library-path L --argv0 A prog`; add `--preload dn-shim.so` (glibc >= 2.30).
  No `dn-interp` hop, no `LD_PRELOAD` in the environment, every exec reloads it.
- **Only read-only calls take the lane**: `openat` without `O_CREAT`/`O_TRUNC`/write
  access, the `stat` family, `readlinkat`, `faccessat`, `getcwd`. Mutating calls,
  `execve` and the id calls always go to `dn-tiny`, so its resolution cache and fake
  root state stay right.
- **One resolver**: the shim links the same `policy.c` and reads the same address
  table, so both paths see one tree.
- **Interpose above the syscall wrappers**: libc-internal calls carry no cookie and stay
  traced, so the shim must cover `realpath` / `canonicalize_file_name` (resolved in the
  shim, no per-component `readlink`), `fopen*`, `opendir`, `fstat`.

## Shim & tracer hardening

**Status**: the libc-interposition layer is complete for its scope
([`docs/spec/runtime.md`](docs/spec/runtime.md)); NSS
and raw-syscall/static binaries route to `dn-trace`. `proot` is gone.

**Open**:

- **Remaining Gate-A syscalls**: `seccomp.c`'s `default:` branch still
  answers these with `ENOSYS` plus a `blocked syscall …` warning — give each
  an emulate-or-refuse decision
  ([`known-issues.md`](docs/reference/known-issues.md), "Gaps toward a
  complete Debian"): `sethostname`, `keyctl`/`add_key`, `io_uring*`,
  `clone3`/`rseq`, `mq_open`, `pidfd_*`, `mbind`/`*_mempolicy`, and the SysV
  `msg*`/`sem*` groups.
- **POSIX `/dev/shm`**: absent on Android, so `shm_open`/`sem_open` fail;
  fake it in dn-policy the way `statfs` already reports it as tmpfs.
- Bake the shim into installed ELFs
  ([`docs/spec/runtime.md`](docs/spec/runtime.md), "Delivering
  the shim") so it survives an empty environment — `DT_AUDIT`/`--add-needed`,
  or `ld.so --preload`.
- Test `dn-run` -> `dn-trace` from an installed prefix, not just a checkout.
- **Post-install analysis pass** (not scheduled): name the public libc calls
  whose *internal* implementation probes a newer syscall and falls back on
  `ENOSYS` — fatal under Android's seccomp (`SIGSYS`). A per-launch check is
  rejected; the shape is a build/install-time analysis pass extending
  `scan-direct-syscalls.py`, then route only those binaries to the tracer.
- **Post-hook non-fatal faults**: a full bootstrap printed a few
  `Segmentation fault` lines from the post hook (it did not reproduce on a
  later run). Investigate.

## Runtime overhaul

`dn-elf` replaced `patchelf` everywhere ([`docs/reference/elf-interp-patch.md`](docs/reference/elf-interp-patch.md)).

**Open**: a name pass over the runtime — any name that no longer describes its
mechanism (`dn-run`'s "launch classifier", ...).

## Backlog (not yet scheduled)

- **Stale `apt.conf` hooks after a repo move**: the prefix's `apt.conf` bakes
  absolute checkout paths; nothing regenerates it for an existing prefix.
- **Run wrappers**: for each binary a package puts on `PATH`, generate a
  wrapper when it needs path help, triggered via apt's `DPkg::Post-Invoke`.
- **Classifier/refusal**: read each `.deb` before dpkg runs, classify scope
  and mechanism, refuse a "never" package before it wedges the prefix.
- **Soname-based dependency matching**: match a `.deb`'s `Depends:` against
  installed packages' SONAMEs directly.
- **Launcher/icon/desktop-DB integration**: only what the host needs.
- **State + `explain` + `doctor`**: record per package its scope, mechanism
  and wrappers, so decisions are explainable/removable.
- **Docs overlap**: `dn-shim-prefix.md` and `dl-mechanics.md` both cover
  run-time prefix self-derivation; consider folding into one.

## Known unsafe, not yet fixed

- The `--force-architecture` workaround for the archive-name mismatch
  (`arm64` vs `aarch64`) needs a real fix.

## Blocked / impossible on this device

Kernel-wide (see [`docs/reference/android-platform.md`](docs/reference/android-platform.md)):
user namespaces off entirely, mount namespaces need `CAP_SYS_ADMIN`,
`/dev/fuse` root-only. Out of scope: install-view / service-view isolation,
seccomp+namespace sandboxing, other-architecture loaders and `Multi-Arch`
skew, setuid/setgid and file capabilities, a TUN device / firewall rules /
raw sockets. SELinux also denies `/proc/sys` and netlink even in a fresh
namespace, so tools that read them fail regardless of the tracer.
