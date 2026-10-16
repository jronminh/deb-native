# TODO

Open work, current state only. How things work: [`docs/architecture.md`](docs/architecture.md);
the numbers: [`docs/performance.md`](docs/performance.md).

## Before an alpha

- **The Android 16 phone**: build a prefix there, run the checks in `AGENTS.md`.
  Hard links, `openat2`, `faccessat2` and `clone3` behave differently there, and
  newer syscalls have no row yet (`open_tree`, `move_mount`, `fspick`,
  `mount_setattr`, `fanotify_mark`, `setxattrat`/`getxattrat`/`listxattrat`/
  `removexattrat`, `file_getattr`/`file_setattr`). Check `execmod` there too: the
  shim's libc patching needs it.
  The S23 FE's kernel is 5.10.240 (`docs/reference/device-census.md`): no SUD,
  no seccomp bitmap; `memfd` exec is refused in Termux's domain.
- **Another phone**, and Termux from F-Droid as a user gets it.
- **dn-shell**: it runs `boot/boot.sh` of a prefix under `prefixes/`, one prefix
  per session, and writes the prefix's `boot/binds` before (see "The
  boundary"); its spec still describes `.dn/contract`.
- **A warning during the second stage**: Android's linker prints "failed to find
  generated linker configuration" once; find which program it is.
- **The default for `-s`**: on in `build-prefix.sh`, off in `boot.sh`. Measure
  `make -j` (the polling tracer competes for a core) and an app session before
  choosing a default for `boot.sh`.

## The boundary

Close what a program meets without asking (`docs/architecture.md`, "The
boundary"; the leaks are listed under "Known gaps" there). In this order, each
step checked as `AGENTS.md` says, in Termux and in the app:

1. **The environment, the umask, the working directory** (`init.sh` and
   `boot.sh` only, no runtime change).
   - Start from an empty environment, as `login` does: `PATH`, `HOME`, `LANG`,
     `TMPDIR`, `TERM` and `COLORTERM`, and what the host asks to keep. **(open)**
     how the host names those: (a) an argument to `boot.sh` (`--keep-env
     TERM,DISPLAY -- CMD`), or (b) a file `boot/env` (a name per line, or
     `NAME=value`), written by the host like `boot/binds`. Leaning to (b): the
     same shape as `boot/binds`, and a Termux user can edit it.
   - `TZ` from the phone (`getprop persist.sys.timezone`). Debian-noroot's
     launcher keeps the same few: `HOME`, `SHELL`, a Debian `PATH`, `TZ`
     (`boot.sh` already drops the host's `LD_PRELOAD` and `LD_LIBRARY_PATH`).
   - `umask 022`, as on Debian (the app's is 077: every new file reads
     `-rw-------`).
   - A command started from a directory outside the tree and its binds starts
     at `/` (`boot.sh` knows the root); today `pwd -P` and `/proc/self/cwd` show
     the host's path.
2. **The SELinux label**: `security.selinux` reads as absent (`ENODATA`, and left
   out of `listxattr`), in the tracer and the shim. The `.` that `ls -l` prints
   after the mode is that label; a Debian without SELinux shows none.
3. **Binds: the law becomes a table.**
   - First, **one copy of the law** (under "The law"): without it a bind is
     written twice, and the 0.7 runtime's two path models disagreed in exactly
     this way.
   - The table: one row per guest path. `/` is the prefix's root; `/proc`,
     `/sys`, `/dev` and the other host directories are rows of their own name;
     binds are rows too. Guest to host by the longest guest prefix; host to guest
     (`policy_unpath`: `getcwd`, `readlink`, `/proc/self/{cwd,exe,fd/N}`) by the
     longest host prefix.
   - `walk` resolves `.`, `..` and links on the guest path and maps at the end:
     `cd /mnt/sdcard/..` is `/mnt`, not `/storage/emulated`, and an absolute
     link inside a bind is the guest's, as under a real bind mount.
   - `boot/binds`: one line per bind, `GUEST<TAB>HOST<TAB>FLAGS` (`rw`; `ro`
     later, answered `EROFS`). Written by the host (dn-shell from its settings,
     a user by hand in Termux) before `boot.sh`; read by the tracer and the shim
     from the root; fixed for a boot. `HOST` must be canonical
     (`/storage/emulated/0`, not `/sdcard`): the shim cannot canonicalize (its
     `realpath` goes through its own hooks), so `dn-tiny` refuses to boot on a
     non-canonical `HOST`, a duplicate `GUEST`, or a `GUEST` under a host row.
   - The mount point: an empty directory at `GUEST`, made at boot if missing, so
     `readdir` of its parent needs nothing; it stays when the bind goes, as after
     `umount`.
   - `rename` and `link` between two rows: `EXDEV`, as between two mounts
     (`prefixes/` and the app's `home/` are one file system, so the kernel would
     allow it). `st_dev` and `statfs` stay the kernel's, as for a bind mount.
   - `/proc/self/mounts`, `/proc/self/mountinfo` (and `/etc/mtab`, a link to the
     first) built from the table, generated at `open` into a memfd ("Emulation").
   - Done when `stat`, `open`, `read`, `readdir`, `readlink`, `getcwd`,
     `realpath` and `pwd -P` agree inside a bind for a dynamic and a static
     program, and `mount`, `df`, `findmnt` list it.
   - Later: `mount --bind` inside the prefix adds a row (it is refused now); the
     table then has to live where the shim sees a change, not a copy read once.
4. **Narrow the host directories**, with binds as the way out: drop `/data`,
   `/storage`, `/sdcard` and `/mnt` from the host rows, and try `/system`,
   `/apex`, `/vendor` (the runtime's own calls use host paths and do not go
   through the law; to check). In the same change, `/proc/*/maps` and `smaps`
   name files by their guest paths: gdb and the JVM open the paths they read
   there, which works today only because `/data` is the host's.

## The law

- **One copy of the law**: the shim has its own resolver (`resolve`,
  `is_host_path` in `lane.c`); link `policy.c` into it instead. It needs a build
  of `policy.c` against glibc (its `sys.h` defines `_start` and the string
  functions) and its link cache made thread-safe. Binds need it first ("The
  boundary", step 3).
- **Invalidating the link cache**: the readlink cache (`policy.c`, `lane.c`)
  drops every entry when the shared generation moves, which any link change of
  any session does (`symlinkat`, `unlinkat`, `linkat`, `rename`). That is coarse
  but cannot miss a change. A finer rule (drop the subtree of a renamed
  directory) is PRoot's issue #124 trie ([`prior-art.md`](docs/reference/prior-art.md),
  "Root emulation and path walks"); measure how often the global drop costs
  before building it.
- **Not mapped yet**: `execveat` (glibc's `fexecve`), `openat2` with
  `RESOLVE_*` (answers `ENOSYS`), `name_to_handle_at`, AF_UNIX socket paths
  (`bind`, `connect`, `sendto`, `sendmsg`; `getsockname`, `getpeername`,
  `accept`, `recvfrom`, `recvmsg` back), so a guest `bind("/tmp/x.sock")` reaches
  the host's `/tmp`: tmux, ssh-agent, gpg-agent, dbus need it.
- **The census against the rows**: `docs/reference/device-census.md` lists the 32
  calls the app's filter kills on the S10e, and each must end in a row (or be
  answered `ENOSYS` on purpose). Check that by hand against `gen_rows.py`. Also
  compare the census with bionic's arm64 app policy (the googlesource path I
  tried returned 404; find the current one).
- **Rows from a catalog**: generate the rows from one table giving each syscall
  a class (path, result, socket, exec, identity, refused, none) instead of the
  tables in `gen_rows.py`. Termux's glibc `fakesyscall.json` is a checked
  list of answers to start from (`docs/reference/prior-art.md`).

## Identity and files

- **An owner store**: `chown` is accepted and not kept, so a file never reads as
  owned by another user (`userdel -r` refuses: "not owned by"). Keep the owner in
  `user.dn.*` xattrs (user xattrs work in the app's data) and answer it in every
  `stat`, in both the tracer and the shim.
- **Hard links that share contents**: a link is a copy with its count
  remembered. A real `link2symlink` would keep one file; do it if a program needs
  the contents shared.
- **Modes**: a user mode (real identity) beside fake root, chosen per command
  (`sudo` turning a process tree to root), for programs that check ownership.
  The user mode needs an `/etc/passwd`, `group` and `shadow` line for the app's
  UID (written at boot when missing), so `whoami`, `ssh` and `$HOME` lookups
  work; `sudo` and `su` then are small scripts that accept the common flags
  (`-A -E -n -S -p`) and switch the tree, as debian-noroot's do with
  `fakeroot-tcp`.

## Speed

- **Keep it small and fast as it grows**: the runtime is 33 KB (`dn-tiny`) and
  77 KB (`dn-lane.so`), and a program in it feels native. Every feature to come
  (binds, SysV, `/proc` files) must leave that so. Move the measuring kit into
  the repo (`tests/bench/`: the regression set, the file-tree set, the Lua
  build, the stop counts; today in a work prefix's `tmp/lane/`), one script that
  prints the rows of `docs/performance.md`, "In an app session", and run it
  before and after each runtime change, interleaved. The filter's order (below)
  is the first change to measure with it.
- **`ld.so`'s own calls** (about 1,100 of the Lua build's 1,700 stops): the
  program, the shim and the libraries are opened before the shim is loaded. The
  exec gate could patch the loader's `svc` sites as the shim does libc's.
- **libc's cancellable path** (324): its `svc` is left alone for
  `pthread_cancel`; a stub that keeps the program counter inside
  `__syscall_cancel_arch`'s range would allow it. Measured
  ([`experiments.md`](docs/reference/experiments.md), E8-E9): on the Lua build
  this is ~341 of the `openat` reads (`open()` through the shim's symbol is
  answered; `fopen()` through this path stops). Located: the cancellable wrappers
  set the number in `x6` and branch to `__syscall_cancel_arch` (one shared `svc`
  for every cancellable call; in the prefix's libc 2.41 it is `bl 0x828ec`,
  `mov x8,x6` at `0x82868`, `svc` at `0x828d8`). One patch covers all of them.
  Find the offset at **build time** (the prefix's libc is known), not at run
  time (`mov x8,x6` is not unique and the function is unexported).
- **Who sets the floor of a stop**: on this phone the tracer's big core sits at a
  low clock while the guest runs on the little cluster. Try the tracer and the
  guest on one cluster, a priority raised with `nice` (no privilege needed down to
  -20), an ADPF hint session from the app. DroidDeck measured one stop at
  6.2 us with tracer and tracee on one core, 15.5 us unpinned, 42-61 us across
  clusters (`docs/development/proot-performance.md` in Droid-Deck/DroidDeck):
  never pin the tracer away from its guests; try both on one cluster.
- **The filter's order** (done 2026-10-16; the Lua build still to measure): the
  filter dispatches on the number first, reads the cookie (`args[5]`) only for
  the lane's numbers, and tests the rows as a JGE tree (`install_filter`,
  `monitor.c`). A `getpid` loop, a number in no row, went from 465 to 355 ms
  per million calls on the S10e (5 interleaved pairs), and a file-tree subset
  from 3.8 to 3.5 s. Results and stop counts of a lane test (the lane's numbers
  with and without the cookie, `openat` with write flags) and of the regression
  set are the same. The test programs are not in the repo yet ("Keep it small",
  above). The kernel's per-number cache (5.11 and later,
  [`prior-art.md`](docs/reference/prior-art.md)) is not on either phone (4.14,
  5.10): the gain is the instructions, not the cache. On a 5.11+ device, check
  that the cache takes the calls the filter now allows.
- **libc's generic `syscall()`**: its `svc` takes the number from a register,
  so the shim's patch (`mov x8,#nr; svc`) misses it. FEX, and programs that
  call `openat` through `syscall()`, stop in the tracer for each path call.
  Route it to the lane for the lane's numbers. svc-hook patches every `svc`,
  which also reaches the loader's own calls (`docs/reference/prior-art.md`).
- **A second tracer for a busy subtree**: one tracer serves every stop of a
  session; DroidDeck measured 2.5x the stops per second with two (a game tree
  apart from Steam's). Only if bursts on many threads show up after the shim.
- **The loader's startup opens** (`ld.so`'s 1082 stops): glibc's `rtld-audit`
  `la_objsearch` hook can rewrite where `ld.so` looks for libraries, as Guix's
  fakechroot engine does ([`prior-art.md`](docs/reference/prior-art.md), "Loader
  and libc interposition"). Measured
  ([`experiments.md`](docs/reference/experiments.md), E4-E6): the loader opens
  the program, the shim and the libraries before the shim's constructor, ~4-9
  stops a process; its `openat`/`newfstatat` sites are patchable and a stub page
  is in range; and `LD_AUDIT`'s `la_version` runs before those opens. So the way
  to remove them is a freestanding audit module that patches the loader, not a
  change to the shim. E7 proved it: a freestanding `LD_AUDIT` module patching
  the loader's `openat` sites at `la_version` drops the stops (`true` 5->4,
  `git --version` 9->6, `python3 -c pass` 15->11). Left: patch `newfstatat` and
  `fstat` too, and measure a full build.
- **The shim's patched pair**: `mov x8,#nr; svc` is right only if no jump lands
  between the two. gVisor (#13362) and ASC-Hook name the same question. Check the
  shim's patch sites for jump targets, and whether `x8` is still the number there.
- **Syscall User Dispatch** is a third net below the tracer, but it needs kernel
  5.11: neither phone has it (4.14, 5.10). Keep it for a 5.11+ device.

## Emulation

Answer what Android refuses in place of the kernel, the same in the tracer and
the shim:

- `/proc` files generated at `open` into a memfd so `read` needs no stop: the
  mount table and `maps` first ("The boundary"); `/proc/sys/*` where a program
  needs an answer Android hides. `/proc/version`, `cpuinfo` and `status` stay
  the kernel's (`docs/architecture.md`, "The boundary"); the files Android 8+
  denies to apps (`/proc/stat`, `loadavg`, `uptime`, `vmstat`: `top`, `uptime`,
  `ps` and `htop` read them) are the first candidates, as Termux's
  proot-distro fakes them;
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
- capabilities (`capget`, `PR_CAPBSET_READ`);
- a clear refusal (`EPERM` or `ENOSYS`, with the reason) for what cannot be
  faked: real isolation, TUN, anything an LSM sees.

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

## Beyond Debian, and bundles (later; the focus is Debian)

The runtime knows nothing of dpkg or apt: a tree with a loader, its libraries
and a program is enough. What still assumes Debian:

- `policy.c` names the loader by Debian's multiarch path
  (`/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1`) and gives it a
  `--library-path` of Debian's directories. Take the loader from the program's
  `PT_INTERP`, resolved in the tree, and let it find libraries through the tree's
  own `ld.so.cache` (its guest paths are mapped like any other): Ubuntu, Arch,
  Fedora trees would then boot as they are.
- `boot.sh` needs `/usr/bin/bash` and `/boot/init.sh`; a tree without a shell
  should run its program directly.
- The exec gate and the shim know glibc only (`ld-linux-*`, `libc.so.6`); musl
  (Alpine) needs the gate's `ld-musl-*` case and a musl build of the shim.
- Ubuntu through debootstrap (`--suite noble --mirror
  http://ports.ubuntu.com/ubuntu-ports`) may work as it is; untested.

**Bundles**: a minimal tree for one program, tens of MB instead of hundreds:
the program, its library closure (`ld.so --list`), glibc and the loader, a few
files it reads (`/etc/ssl/certs`, `/etc/passwd`, `nsswitch.conf`,
`resolv.conf`), `dn-tiny` and `dn-lane.so`, and no dpkg or apt. Made from a full
prefix as a builder (`apt-get download` the package and its dependencies,
`dpkg-deb -x` them, no maintainer scripts): an image, not upgraded in place.
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

Open: whether to write this up as `docs/embedding.md`, and a minimal sample app
(one activity, one button that runs a command in a prefix) to prove the path.

## Building a prefix

- `build-prefix.sh` relies on Termux's `debootstrap` for the first stage, which
  unpacks through `proot --link2symlink`. A first stage of its own (resolve the
  base set, fetch, unpack with hard links as copies) would drop proot from the
  build.
- Suites other than Debian's (Ubuntu, Kali) and `--variant` other than `minbase`
  are untested.
