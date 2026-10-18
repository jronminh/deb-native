# TODO

Open work, current state only. How things work: [`docs/architecture.md`](docs/architecture.md);
the numbers: [`docs/performance.md`](docs/performance.md).

## Before an alpha

- **The Android 16 phone**: the census is done there
  (`docs/reference/device-census.md`: the S23 FE, kernel 5.10.240, 42 real
  denials; stock glibc runs under `dn-tiny`), but a full prefix is not built
  there and the checks in `AGENTS.md` are not run. `openat2`, `faccessat2` and
  `clone3` run natively there. The newer calls (`open_tree`, `move_mount`,
  `fspick`, `mount_setattr`, `fanotify_mark`, `setxattrat`/`getxattrat`/
  `listxattrat`/`removexattrat`, `file_getattr`/`file_setattr`) need no row: the
  platform kills them and `policy_deny`'s `ENOSYS` default answers
  (`tests/bench/syscalls.c` exercises them).
  Check hard links and `execmod` there too: hard links are Android's SELinux
  rule, not the census's, and the shim's libc patching needs `execmod`. No SUD
  and no seccomp bitmap (5.10 < 5.11); `memfd` exec is refused in Termux's
  domain (checked on the S10e).
- **Another phone**, and Termux from F-Droid as a user gets it.
- **dn-shell**: stage 5 (prefixes) is next -- it does not run a prefix's
  `boot/boot.sh` yet, and will write the prefix's `boot/binds` before (see "The
  boundary"); its spec still describes `.dn/contract`. The host contract is
  [`docs/spec/host.md`](docs/spec/host.md).
- **A warning during the second stage**: Android's linker prints "failed to find
  generated linker configuration" once; find which program it is.

## The boundary

Close what a program meets without asking (`docs/architecture.md`, "The
boundary"; the leaks are listed under "Known gaps" there). In this order, each
step checked as `AGENTS.md` says, in Termux and in the app:

1. **The environment, the umask, the working directory** (done; `init.sh` and
   `boot.sh` only, no runtime change).
   - `boot.sh` starts the command from an empty environment (`env -i`) with
     `PATH`, `HOME`, `SHELL`, `LANG`, `TMPDIR`, `TERM`, `COLORTERM` and `TZ`
     (the phone's, `getprop persist.sys.timezone`); the host's `PATH`,
     `ANDROID_*` and the rest do not pass.
   - `umask 022` (`boot.sh` and `init.sh`), as on Debian (the app's 077 made
     every new file `-rw-------`).
   - A command starts in the tree's root (`cd "$root"` in `boot.sh`), so `pwd -P`
     and `/proc/self/cwd` read `/` -- before, the host's working directory was
     shown.
   - **`boot/env`** (done): one `NAME` a line keeps the host's value, one
     `NAME=VALUE` sets it; `boot.sh` reads it as data (never sources it) and a
     bad name refuses the boot.  It overrides the defaults it names.
   - The coverage test (`scripts/coverage.sh`) no longer flags the working
     directory or the environment.
2. **The SELinux label** (done): `security.selinux` reads as absent -- `getxattr`
   answers `ENODATA` (`-61`), `listxattr`/`llistxattr` drop it and report the
   shorter length, in the tracer (answered at entry) and the shim. `ls -l` no
   longer prints the `.` after the mode. The fd forms (`fgetxattr`, `flistxattr`)
   are answered the same way.
3. **Binds: the law becomes a table.** Done (the one leftover, `ro`, is in).
   - **The table** (done): one row per guest path. `/` is the prefix's root;
     `/proc`, `/sys`, `/dev` and the other host directories are rows of their own
     name; binds are rows too. Guest to host by the longest guest row, host to
     guest (`policy_unpath`: `getcwd`, `readlink`, `/proc/self/{cwd,exe,fd/N}`)
     by the longest host row. `policy_is_host` is the identity rows, not a
     keep-list.
   - **`walk` in guest space** (done): it resolves `.`, `..` and links on the
     *guest* path and maps at the end, so `cd /host/..` is `/`, not the host
     parent, and an absolute link inside a bind is the guest's. (A bind under a
     host row is refused; since step 4 dropped `/mnt`, it is the tree's now.)
   - **`boot/binds`** (done): one line per bind, `GUEST<TAB>HOST<TAB>FLAGS`.
     Written by the host before `boot.sh` and read by the tracer
     (`policy_set_root`) and the shim (`lane_init`) from the root; fixed for a
     boot. The tracer refuses to boot on a non-canonical `HOST`, a duplicate
     `GUEST`, or a `GUEST` under a host row. A `ro` row answers `EROFS` on a
     write: the tracer for a path argument (`openat` with a write flag,
     `unlinkat`, `rename`, `linkat`, ...) and the shim for the calls it takes on
     the lane (`mkdirat`, `[l]setxattr`, `truncate`, `fchmodat`, `utimensat`).
   - **The mount point** (done): `dn-tiny` makes an empty directory at each
     bind's `GUEST` at boot (`mkdir -p` the components), so `readdir` of its
     parent sees it; it stays when the bind goes, as after `umount`.
   - **`rename` and `link` between two rows** (done): `EXDEV`, decided on the
     guest paths at entry (a host-path check is ambiguous: the root may sit under
     a host row, or a bind's host under the root). `st_dev` and `statfs` stay the
     kernel's (true already).
   - **The mount view** (done): `policy_mounts` builds the table once, at boot,
     into a file under `var/lib/deb-native` (a memfd cannot be read by the child
     across the ptrace boundary: `/proc/<tracerpid>/fd/N` answers `EACCES`). The
     names `/proc/mounts`, `/proc/self/mounts`, `mountinfo`, `/proc/self/mountinfo`
     and `/etc/mtab` are **rows** (`B_SYNTH`) that map straight to that file, with
     no walk -- `/proc/self` is a magic symlink the kernel resolves to the
     tracee's own pid, so walking it would miss the row. A regular file is read
     with no stop. `mount`, `df` and `findmnt` list the root and each bind; the
     source is `deb-native`, never a host path. Left: `/proc/<pid>/mounts` for an
     explicit **other** pid still shows the kernel's (no static row can name a
     dynamic pid), and `/proc/sys/*` where a program needs another answer
     ("Emulation").
   - Later: `mount --bind` inside the prefix adds a row (it is refused now); the
     table then has to live where the shim sees a change, not a copy read once.
4. **Narrow the host directories** (done for the rows): the table keeps only
   `/proc`, `/sys` and `/dev` as host rows; `/data`, `/storage`, `/sdcard`,
   `/mnt` and (tried, and dropped) `/system`, `/apex`, `/vendor` are the tree's
   now -- a path there answers `ENOENT` (a Debian has no such directory), not the
   phone's, and a bind in `boot/binds` is the way to reach one on purpose.  The
   runtime's own calls use host paths and do not go through the law, so they are
   unaffected.  `/proc/*/maps` and `/proc/*/smaps` are generated at open too, the
   file paths mapped host->guest (gdb and the JVM read the names they open).

## The law

- **Invalidating the link cache**: the law's readlink cache (`policy.c`, linked
  into the shim and shared by both engines, a seqlock) drops every entry when the
  shared generation moves, which any link change of any session does
  (`symlinkat`, `unlinkat`, `linkat`, `rename`). That is coarse but cannot miss a
  change. Measured: putting the link movers in the shim, which then has to move
  the same generation, cost the Lua build +46 stops
  ([`policy-audit.md`](docs/reference/policy-audit.md)) -- one place is cheaper
  than two. A finer rule (drop the subtree of a renamed directory) is PRoot's
  issue #124 trie ([`prior-art.md`](docs/reference/prior-art.md), "Root emulation
  and path walks").
- **AF_UNIX socket paths** (done): `bind`, `connect`, `sendto`, `sendmsg` carry a
  `sockaddr_un`, and `getsockname`, `getpeername`, `accept`, `recvfrom`, `recvmsg`
  fill one; the tracer maps the `sun_path` guest->host (in) and host->guest (the
  exit stop), the length register or the `msghdr`'s `msg_namelen` adjusted. A
  guest `bind("/tmp/x.sock")` now makes the file in the tree. tmux, ssh-agent,
  gpg-agent and dbus can meet.
- **Refused on purpose** (a mapping would be a lie): `openat2` with `RESOLVE_*`
  answers `ENOSYS` (without a resolve flag it runs as `openat`, the path mapped);
  and minor, privileged (allowed, a guest path reaches the kernel): `pivot_root`,
  `quotactl`. `execveat` is done (a row through the exec gate; `fexecve` works).
  The SELinux xattr is read absent, the fd forms (`fgetxattr`/`flistxattr`) too.
- **Two small refusals**: `acct` (89) and `__reboot` (142) are in bionic's app
  blocklist, so the platform kills them; `policy_deny` has no case and answers
  the default `ENOSYS`.  Answer `EPERM`, as a non-root would get (the census
  skipped both because they change global state; see
  [`seccomp-gates.md`](docs/reference/seccomp-gates.md)).
- **The census against the policy** (the S10e half is done): the 32 calls the
  app's filter kills are accounted for in `docs/reference/device-census.md`
  (8 `FAKE`, `accept` -> `accept4`, 17 `EPERM`, 6 `ENOSYS`; the default answer
  covers a new denial). The per-call map (transform / placement / validation,
  and what is a fast path) is `docs/reference/syscall-classes.md`; keep it in
  step when a row or an answer changes. Also compare the census with bionic's
  arm64 app policy (the googlesource path I tried returned 404; find the
  current one).
## Identity and files

- **Hard links that share contents**: a link is a copy with its count
  remembered. A real `link2symlink` would keep one file; do it if a program needs
  the contents shared.
- **Identity** (the model and current state:
  [`docs/spec/identity.md`](docs/spec/identity.md)): one real principal (the
  app's uid) and virtual principals; the boot seeds the real uid by default
  (`DN_INITIAL=root` for a root session), and every syscall is classified by
  the process's euid -- no mode switch.  The setuid-bit gate is in (a stored
  owner of 0 or the real uid; no store is root's), with the `DN_IDS` hint for the
  shim; a create is marked with the creator's ids (`openat`+`O_CREAT`,
  `mkdirat`, `mknodat`); supplementary groups are a per-process virtual list
  (`{0}` for root, the app's groups for the real user, replaced by
  `setgroups`/`initgroups`, no fixed cap).  The fake root's DAC override is
  emulated (a write open adds owner-write, restored at exit), `NoNewPrivs`
  reads 0, `PR_SET_DUMPABLE` is ignored, an unmarked file (legacy, or a
  symlink) reads `0:0` in both worlds, and `access()`/`faccessat` answer the
  virtual view.  With those, real `sudo`, `su` and `passwd` work (sudo NOPASSWD
  with `apt-get` and `useradd`; su login and password sessions; passwd as root
  and as the user proving the old password), and a `useradd`/`adduser` wrapper
  says "virtual user".  Still open: the kernel's own open still checks the
  app's gid (group access is a view), capabilities, and binds from the host.
  Virtual users are not Android users and are not isolation.

## Speed

- **Keep it small and fast as it grows**: the runtime is about 72 KB (`dn-tiny`)
  and 77 KB (`dn-lane.so`), and a program in it feels native. Every feature that comes
  (a new bind, IPC, a `/proc` file) must leave that so. `scripts/bench.sh` runs real
  workloads (a stat storm, links, a git workflow, Python, a tar round trip, a
  spawn loop); the lane test, the regression set and the Lua build's wall now
  live in `tests/bench/` (`run.sh` prints the rows of `docs/performance.md`,
  "In an app session"). Run it before and after each runtime change,
  interleaved.
- **`ld.so`'s own calls** (about 1,100 of the Lua build's 1,700 stops): the
  program, the shim and the libraries are opened before the shim is loaded. Done
  by the loader hook (`src/dn-tiny/ldhook.c`, an `LD_AUDIT` module): it patches
  the loader's `svc` sites and now maps the loader's guest paths through the one
  law -- the Lua build drops from 1670 to 848 stops. Not shipped by default (a
  prefix ships it only when `usr/lib/deb-native/dn-ldhook.so` is present); it
  needs `execmod`/`execmem`, so confirm on the Android 16 phone before it ships.
- **libc's cancellable path** (324 stops, done as an option): its `svc` is left
  alone for `pthread_cancel` (which reads the program counter there).  The
  wrappers set the number in `x6` and branch to `__syscall_cancel_arch` (one
  shared `svc`; in libc 2.41, `mov x8,x6` at `0x82868`, `svc` at `0x828d8`).
  `build-prefix.sh --cancel` finds that offset at build time
  (`scripts/find-cancellable.py`) and passes it to the shim: the Lua build falls
  from 866 to 662 stops and cancellation still works, but the wall is ~1%
  slower (14.2 s vs 14.1 s interleaved) because the hook taxes every
  cancellable call.  Off by default; the runtime `mov x8,x6` heuristic stays
  rejected (E10: no stable contract).
- **Who sets the floor of a stop**: on this phone the tracer's big core sits at a
  low clock while the guest runs on the little cluster. Try the tracer and the
  guest on one cluster, a priority raised with `nice` (no privilege needed down to
  -20), an ADPF hint session from the app. DroidDeck measured one stop at
  6.2 us with tracer and tracee on one core, 15.5 us unpinned, 42-61 us across
  clusters (`docs/development/proot-performance.md` in Droid-Deck/DroidDeck):
  never pin the tracer away from its guests; try both on one cluster.
- **On a 5.11+ device, check the filter's per-number cache**: the filter reads an
  argument (`args[5]`, the cookie) only for a row the lane also answers, and is
  one JGE tree otherwise (`install_filter`, `monitor.c`; the numbers are in
  [`performance.md`](docs/performance.md)), so a call in no row and no lane
  should take the kernel's per-number cache (5.11 and later,
  [`prior-art.md`](docs/reference/prior-art.md)). Neither phone has it (4.14,
  5.10); confirm on a 5.11+ device that the calls the filter allows are cached.
- **libc's generic `syscall()`**: its `svc` takes the number from a register,
  so the shim's patch (`mov x8,#nr; svc`) misses it. FEX, and programs that
  call `openat` through `syscall()`, stop in the tracer for each path call.
  Route it to the lane for the lane's numbers. svc-hook patches every `svc`,
  which also reaches the loader's own calls (`docs/reference/prior-art.md`).
- **A second tracer for a busy subtree**: one tracer serves every stop of a
  session; DroidDeck measured 2.5x the stops per second with two (a game tree
  apart from Steam's). Only if bursts on many threads show up after the shim.
- **The shim's patched pair**: `mov x8,#nr; svc` is right only if no jump lands
  between the two. gVisor (#13362) and ASC-Hook name the same question. Check the
  shim's patch sites for jump targets, and whether `x8` is still the number there.
- **Syscall User Dispatch** is a third net below the tracer, but it needs kernel
  5.11: neither phone has it (4.14, 5.10). Keep it for a 5.11+ device.
- **The fast paths are manipulations of the syscall instruction.** The room the
  "stable net, fast path" rule opens (`mapped-kernel.md`, rule 7) is mostly
  here: rewriting a `svc` into a call to a handler (zpoline's shape, on arm64),
  one trampoline per number (gVisor's specialization), a first-call trap that
  patches the site it learns (systrap; E12), the cancellable `svc`, the generic
  `syscall()` whose number is in a register, the loader's own sites. Each is
  fast and each needs its own fail-safe. The one that best fits "rest on stable
  things" learns a site from a **real trap** instead of guessing it from the
  codegen -- which is what E12 tried; its block (a handler that does not survive
  `exec`) is the thing to solve.
- **Kernel mechanisms to lean on, and what the platform takes away**:
  [`reference/kernel-mechanisms.md`](docs/reference/kernel-mechanisms.md) --
  seccomp user notification (+ `ADDFD`), `pidfd`, `process_vm_*`, `inotify`,
  vDSO. The useful ones are 5.0+/5.9+, so today the S23 FE; the S10e (4.14) has
  only `ptrace` and `RET_TRAP`.

## In-process: the catcher into the tracee

Today the catcher is `dn-tiny`, a separate process reached by `ptrace`: the shim
is the in-process fast path, the tracer the net (the five blind spots, the
`ld-dn-shim` design; [`experiments.md`](docs/reference/experiments.md), E4-E8).
A row the shim cannot reach costs a stop, and E11 shows the stop **is** the cost.
The idea is to move the catcher **into the process** for the programs that can
carry it, so a row costs no stop. An experiment did this (E12/E13); its findings
are kept here and in `experiments.md`, and its branch has been dropped.

- **Same law, same contract.** `policy.c` and `lane.h` do not change; only
  *where* they run does. The move is a role move, not a rewrite.
- **Two mechanisms, to weigh first** ([`inline-hooking.md`](docs/reference/inline-hooking.md)):
  1. **`svc`-site patching, extended** (svc-hook): the shim already patches
     libc's `svc` and answers in-process; patch every row it can reach
     (`execve`, the cancellable path of E9-E10, ...). No seccomp change. Hard
     part: finding the sites (E10: a mis-identified site can corrupt libc).
  2. **`seccomp RET_TRAP` + `SIGSYS` handler** (systrap): the shim installs a
     second filter that `TRAP`s the rows (higher precedence than the tracer's
     `TRACE`, so the tracer is not woken) and a `SIGSYS` handler that does what
     `trace_enter`/`trace_denied` do, re-issuing with the cookie. Reaches any
     site. **Tried, blocked** ([`experiments.md`](docs/reference/experiments.md),
     E12): a seccomp filter survives `exec` but the handler resets, so a program
     exec'd from a shimmed parent traps the loader's own `openat` with no
     handler and is killed. It needs a handshake (the tracer answers until the
     shim's handler is up) or another route.
- **What stays the tracer's:** static and raw-`svc` programs (blind spots 4-5),
  and the boot (install the filter, the first exec gate).
- **Phases:** (1) one row (`openat`) in-process in a dynamic program; measure
  the stop count (`dn-tiny` without `-q`). (2) the rest of the rows. (3) the
  exec gate. (4) decide static.
- **Test:** the Lua build and the regression set ([`performance.md`](docs/performance.md)),
  interleaved; a threaded program and `pthread_cancel` (E10).

## Emulation

- **The contract** ([`docs/spec/contract.md`](docs/spec/contract.md)): the
  decision frame -- two gates (seccomp and the kernel), three levels (promised,
  best-effort, refused).  T1 (base and dev) is the milestone and is in; the
  feature set is frozen there.  T2 (the shared-memory broker: SysV shm/sem/msg,
  POSIX shm) is an **open question**, not planned: it is the expensive quadrant
  (seccomp-killed + config off), and a named workload plus a time-boxed spike is
  the way in.  See [`docs/reference/seccomp-gates.md`](docs/reference/seccomp-gates.md).

Answer what Android refuses in place of the kernel, the same in the tracer and
the shim:

- `/proc` files answered from the table so `read` needs no stop: the **mount
  table** (`mounts`, `mountinfo`, `/etc/mtab`) and **`maps`/`smaps`** are done
  (rows mapping to a file under `var/lib/deb-native`, and generated at open --
  "The boundary", steps 3-4); the process view (`status`, `cmdline`, `comm`,
  `cgroup`) is done too (`docs/spec/phase2.md`). Left: `/proc/sys/*` where a
  program needs an answer Android hides -- three defaults are answered
  (`kernel/pid_max`, `vm/max_map_count`, `kernel/cap_last_cap`). `/proc/version`
  and `cpuinfo` stay the kernel's
  (`docs/architecture.md`, "The boundary"). The files Android 8+ denies to apps
  (`/proc/stat`, `loadavg`, `uptime`, `vmstat`): `/proc/uptime` and
  `/proc/loadavg` are answered with the **real** values (`CLOCK_BOOTTIME` and
  `sysinfo`; `uptime` works) and generated at open. `/proc/stat` and
  `/proc/vmstat` have no real source, so they get a **fixed placeholder** (all
  counters 0, `btime 0`; a `dn_not_faked 1` key) -- readable, and clearly not the
  kernel's, instead of a refused `EACCES`. Replace the placeholder if a real
  source is ever found.
- SysV IPC (`shmget`, `shmat`, `shmdt`, `shmctl`, then `semget`/`semop` and `msgget`),
  two designs to choose from: a broker over a unix socket with `SCM_RIGHTS`
  (Winlator's glibc patches), or the calls in the tracer (proot's `--sysvipc`
  branch, msg and sem first, shm later) ([`prior-art.md`](docs/reference/prior-art.md),
  "System V IPC"),
  killed by the platform today: a segment is a `memfd`, its key and id kept by
  a broker in the tracer that hands the fd over a unix socket, `shmat` an
  `mmap` of it, in the shim. Debian-noroot's `android-shmem` does the same
  with ashmem for X's MIT-SHM; PostgreSQL's postmaster needs a small segment
  too. Steam needs semaphores with the real semantics (a `semop` waiting is
  woken when another process changes the set): huntergdavis/steamclienttermux
  names that, with robust lists, as what made Steam run. Droid-Deck/DroidDeck's
  `tools/linuxfs/preload/sysv.c` (GPL-3.0, as this repo) keeps shm, sem and msg
  as files under `/dev/shm`. Do it when a program needs it;
- privileged ports (a bind translation, and `getsockname`/`accept` back);
- capabilities: the `Cap*` sets in `/proc/<pid>/status` and `cap_last_cap` are
  answered; `capget` and `PR_CAPBSET_READ` are not;
- a clear refusal (`EPERM` or `ENOSYS`, with the reason) for what cannot be
  faked: real isolation, TUN, anything an LSM sees.

## Android, inherited (later)

The kernel is the phone's, not an emulated one, so its own facilities are
reachable on purpose -- not only the calls Android refuses, but what it adds
over a PC's kernel: `binder` and `ashmem`, the GPU nodes (`/dev/kgsl-3d0`,
`/dev/ion`/`/dev/dma_heap` -- "Graphics"), the Android netlink families. A bind
(`boot/binds`) exposes a node and `ioctl` is never stopped, so a glibc program
can drive it from the tree. Worth reaching for when a program wants one (a GPU;
an IPC or shared-memory client); today they are only what a prober finds
([`docs/architecture.md`](docs/architecture.md), "Inherited from Android").

## Graphics (later)

What the Steam-on-Android projects of 2026 do (Droid-Deck/DroidDeck,
phobos665/SteamOS-Lite, huntergdavis/steamclienttermux; all under PRoot), for
when a prefix should draw with the GPU:

- **The GPU from the prefix**: Mesa's Turnip (Vulkan for Adreno) talks to
  `/dev/kgsl-3d0` directly from glibc, no bionic in between; OpenGL is Zink on
  Turnip. The S10e has an Adreno 640 (a6xx, supported), `/dev/kgsl-3d0` is
  `rw-rw-rw-`, and its buffers come from `/dev/ion` (`/dev/dma_heap` on newer
  kernels). Needs: those nodes kept visible (step 4 of "The boundary"),
  `ioctl` never stopped (every GPU submit is one), and probably a Mesa built
  with `-Dfreedreno-kmds=kgsl` (check what Debian's `mesa-vulkan-drivers`
  enables). DroidDeck's preloads show the KGSL quirks to expect: `drm.c`
  (GEM handles over KGSL for dma-buf sharing), `kgslid.c` (a chip id fix),
  `kgslpoll.c` (timestamp waits).
- **The screen**: the app is a Wayland compositor (DroidDeck's
  `app/src/main/cpp/waylandcomp`, drawing on a SurfaceView with a bionic
  Turnip through adrenotools); clients hand dma-bufs over
  `zwp_linux_dmabuf_v1`, and the compositor blits each frame once. One
  `ASurfaceControl` layer per window with `AHardwareBuffer`s drops the copy
  (about 20% more FPS measured) but needs a patch to Mesa's Wayland WSI
  (`ZERO_COPY_SPIKE.md` there). This is the shape for dn-shell: the app the
  compositor, the prefix its clients; X programs through Xwayland. The other
  way, Termux:X11, broke Chromium's GPU compositing (steamclienttermux keeps
  CEF in software).
- **GPUs other than Adreno** (Mali, Xclipse): no open driver reaches them
  from glibc. huntergdavis/bionic-vulkan-bridge forwards Vulkan from a glibc
  client over a unix socket to a bionic service using the system's
  `libvulkan.so`; compute works there, display does not yet.

## Beyond Debian, and bundles (the focus is Debian)

The runtime knows nothing of dpkg or apt: a tree with a loader, its libraries
and a program is enough. The variants worth building, a worked busybox bundle and
what each still names Debian: [`spec/variants.md`](docs/spec/variants.md). The
busybox bundle is built and runs (`scripts/build-busybox.sh`: 7 MB, no Debian
userland, its own `boot.sh` under `busybox sh`). A from-source tree (LFS native),
built by a prefix as its host: that and the from-scratch references are in
[`reference/from-scratch.md`](docs/reference/from-scratch.md).
What still assumes Debian:

- `boot.sh` needs `/usr/bin/bash` and `/boot/init.sh`; a bundle writes its own
  `boot.sh` (the busybox one does), and a tree without a shell should run its
  program directly.
- The exec gate and the shim know glibc only (`ld-linux-*`, `libc.so.6`); musl
  (Alpine) needs the gate's `ld-musl-*` case and a musl build of the shim.
- Ubuntu (`--suite noble --mirror http://ports.ubuntu.com/ubuntu-ports`)
  builds, boots and runs apt (2026-10-17; `--distro` follows the mirror's name).
  Arch and Fedora are untested.

**Bundles**: a minimal tree for one program, tens of MB instead of hundreds:
the program, its library closure (`ld.so --list`), glibc and the loader, a few
files it reads (`/etc/ssl/certs`, `/etc/passwd`, `nsswitch.conf`,
`resolv.conf`), `dn-tiny` and `dn-lane.so`, and no dpkg or apt. Made from a full
prefix as a builder (`apt-get download` the package and its dependencies,
`dpkg-deb -x` them, no maintainer scripts): an image, not upgraded in place.
`build-busybox.sh` is that shape for one tree; a generic `build-bundle.sh` is
not written.
A bundle needs no fake root (fewer rows); the shim built in the builder works on
the same or a newer glibc; a static program (Go, Rust with musl) needs only
`dn-tiny`. For an app that embeds a tool (ffmpeg, python, node), or a glibc
binary such as Claude Code.

## Open question: a prefix in any app

dn-shell is one frontend, not the platform: a prefix's whole contract is
`boot/boot.sh`, so any app that can start a child process can run one, with no
terminal (a script, a local server, a tool an app embeds; with a bundle, tens of
MB). What it takes, as far as known:

- **targetSdk 28 or lower**, as things stand. From API 29 the app domain may not
  `execve` files in its own data directory, where the prefix's loader and
  libraries live (Termux stays at 28 for the same reason). Google Play requires
  a much newer target, so such an app ships outside it (F-Droid, GitHub, by hand).
- **A way past it, to try: a trampoline in the APK.** Believed, not checked: at
  API 29+ only the `execve` is refused; `mmap(PROT_EXEC)` of a data file is still
  allowed (`dlopen` of a downloaded `.so`, Termux's `linker64` trick), and so is
  an `execve` of the APK's native library directory (`apk_data_file`). The
  exec gate already rewrites every `execve` into "run the loader on the
  program"; only its target changes:
  - ship `dn-tiny` and a small `dn-exec` as `libdntiny.so`, `libdnexec.so` in
    the APK's `jniLibs` (`extractNativeLibs="true"`);
  - the gate turns `execve(PROG)` into `execve(APKLIB/libdnexec.so, ...)`;
    `dn-exec`, a userland exec, maps the **prefix's own** `ld-linux-aarch64.so.1`,
    builds the stack and auxv, and jumps into it as `ld.so PROG`; a static
    program it maps directly. Not glibc's loader in the APK: `ld.so` and
    `libc.so.6` are a pair, and `apt upgrade libc6` would split them;
  - `/proc/self/exe` is already answered with the program (`exe_now`).
  - The shim's libc patching needs `execmod`, likely refused at API 29+: copy
    the text pages to anonymous memory, patch, `mprotect` RX and `mremap` them
    over the file mapping instead (needs `execmem`, which apps have for JIT; to
    check).
  - The runtime then ships with the APK (an update of `dn-tiny` is an app
    update); `boot.sh` keeps working in Termux at 28 the current way.
  - If a later Android refuses executable mappings of data files too, as it did
    for dex in 14, what is left is copying code into anonymous memory (`execmem`).
  - The checks first, in a `dev` build of dn-shell at `targetSdk 34` on the
    Android 16 phone, from a session of the app: `execve` of a file in `files/`
    (expected: refused); `/system/bin/linker64` on a bionic binary in `files/`
    (expected: runs); `mmap(PROT_EXEC)` of a file in `files/`, and `mprotect`
    RX of anonymous memory (`execmem`), each in a small program; and `fexecve`
    of a `memfd`. In Termux's domain (`untrusted_app_27`, targetSdk 28) the last
    one already fails with `EACCES` where the same ELF from a file runs
    (`device-census.md`, "Two more checks"); the targetSdk 34 result is the one
    that matters. The `memfd_file` class in sepolicy (commit `d4564b29`) is the
    rule to read for it.
- arm64; `ptrace` of its own children and `execmod` (both allowed in
  `untrusted_app_27`).
- The app extracts the prefix into its files directory (a prefix names no
  absolute path and holds no hard links), runs `ROOT/boot/boot.sh CMD...`
  (`/system/bin/sh`) or `dn-tiny -t ROOT -- ...` directly, and keeps a foreground
  service for long work.
- Testing must happen inside the app's own process: `run-as` runs in
  `runas_app`, another domain.

The host side is written up as [`docs/spec/host.md`](docs/spec/host.md) (a
proposal; [`docs/spec/boot-manager.md`](docs/spec/boot-manager.md) for the
launcher). Open: a minimal sample app (one activity, one button that runs a
command in a prefix) to prove the path, and whether it becomes
`docs/embedding.md`.

## Building a prefix

- `build-prefix.sh` relies on Termux's `debootstrap` for the first stage, which
  unpacks through `proot --link2symlink`. A first stage of its own (resolve the
  base set, fetch, unpack with hard links as copies) would drop proot from the
  build.
- Ubuntu builds and boots (2026-10-17); Kali, other suites and `--variant`
  other than `minbase` are untested.
