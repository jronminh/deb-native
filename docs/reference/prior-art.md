# Prior art: who else answers Linux one syscall at a time

Projects that do a part of what deb-native does, and what each one offers it.
None of them combines the three parts deb-native rests on
([`architecture.md`](../architecture.md)): an unmodified Debian glibc tree,
a shim that patches the `svc` sites in the process, and a tracer as the net
under it, both answering by one law. Checked 2026-10-15 and 2026-10-16, with the
platform rows added 2026-10-19; the claims about a project's own behavior come
from its README or paper, and are not checked on this phone unless the text says
so. The long ideas behind them,
with their papers, are in [`research.md`](research.md); the security lens on
restricted syscalls is in [`syscall-security.md`](syscall-security.md).

## Contents

- [The closest relative: WSL1](#the-closest-relative-wsl1)
- [On Android](#on-android)
- [Patching `svc` on arm64](#patching-svc-on-arm64)
- [Syscall User Dispatch](#syscall-user-dispatch)
- [Trap and patch elsewhere](#trap-and-patch-elsewhere)
- [seccomp filters and their cost](#seccomp-filters-and-their-cost)
- [Userland exec](#userland-exec)
- [Exec on Android: W^X and the linker](#exec-on-android-wx-and-the-linker)
- [Root emulation and path walks](#root-emulation-and-path-walks)
- [Loader and libc interposition](#loader-and-libc-interposition)
- [System V IPC and shared memory](#system-v-ipc-and-shared-memory)
- [Other hosts, devices, and ABI](#other-hosts-devices-and-abi)

## The closest relative: WSL1

WSL1 ran unmodified Linux programs on the NT kernel: each Linux process was a
"pico process", and a kernel driver (`lxcore.sys`) answered its syscalls one
by one, translating paths, identities and `/proc` onto NT. deb-native is the
same idea with a Linux kernel underneath: most calls pass through untouched,
and only those Android refuses or that name a path are answered, by the shim
in the process or by the tracer. WSL1's lessons hold here too: per-call
emulation is fast where the host kernel already agrees, and every gap
(`/proc`, `mount`, ownership) is emulated one at a time. Gramine, a library
OS, is the in-process form of the same idea: its glibc is rebuilt to call the
library OS instead of the kernel, where deb-native patches an unmodified one.

## On Android

| project | what it does | what deb-native can take |
| --- | --- | --- |
| [Droid-Deck/DroidDeck](https://github.com/Droid-Deck/DroidDeck) | PRoot plus a preloaded "fast path" answering `open`/`stat`/`access`/`readlink` in the process through a trampoline page the filter lets through (prototype, `docs/development/proot-performance.md`) | measurements of a stop by CPU placement and of the seccomp filter cache; its preloads (`tools/linuxfs/preload/`) as a catalog of emulations: SysV, `futex_waitv`, robust lists, udev and input, userspace binfmt. GPL-3.0 |
| [brunodev85/winlator](https://github.com/brunodev85/winlator) | Windows x86/x64 on Android: Wine, Box86/Box64 (CPU translation), DXVK/VKD3D, Mesa/Turnip; each container is a rootfs (PRoot) with its own config (graphics driver, env vars, component versions); the Android glue is the app's -- a `GPUImage` over `AHardwareBuffer` + EGL image + a dma-buf fd, an XServer display activity, an ALSA plugin, input profiles; a client-server-over-a-socket pattern runs through SysV shm (`sysvshm_server`), audio and even GL/Vulkan (`gladio`, `vortek`) | the per-container config model (env vars and component versions: a mature `boot/env`/`boot/binds`); the buffer path to an app Surface (`AHardwareBuffer`/dma-buf, "GPU Image and Hardware Buffer Management"); the client-server-over-a-socket shape for what the kernel will not give. Its CPU emulation and its GL/Vulkan translation are what a native arm64 tree (Mesa/Turnip on kgsl) does not need. LGPL-2.1 |
| [termux-pacman/glibc-packages](https://github.com/termux-pacman/glibc-packages) | a glibc rebuilt so it never makes the calls Android kills: `gpkg/glibc/fakesyscall.json` maps each one to its answer (`faccessat2` → `faccessat`, `statx` → a generic `statx`, the `set*id` family → 0, `shm*` → an ashmem emulation) | the table, checked on Android, as a source for the rows (TODO, "Rows from a catalog"); no path translation |
| [pelya/debian-noroot](https://github.com/pelya/debian-noroot) | PRoot, `fakeroot-tcp` behind `sudo`, a preload stubbing libselinux and xattrs and emulating SysV shm over ashmem (`android-shmem`) | the environment it keeps, a `passwd` line for the app's UID; its 2012 fakechroot attempt (preload only) failed, as a preload alone does |
| [huntergdavis/steamclienttermux](https://github.com/huntergdavis/steamclienttermux) | Steam on Termux glibc outside PRoot, games under a patched PRoot | robust lists and SysV semaphores with real semantics are what Steam needs |
| [emontiel-dev/proroot](https://github.com/emontiel-dev/proroot) | rootless runtime for Android (API 26+, arm64): an `LD_PRELOAD` runtime translating paths in-process, and binary patching of `svc #0` into a trampoline so glibc-internal raw syscalls are caught too; runs an Ubuntu arm64 rootfs with Node, Python, Git, Chromium (its README; unchecked) | the closest in-process design to the shim, built on the same `svc` patch; its README claims zero ptrace stops and reports a bootstrap bug in v1.0.12, which is worth reading against the TODO, "Speed" |
| [sylirre/arm64emu-user](https://github.com/sylirre/arm64emu-user) | a user-space AArch64 emulator for chroots on Android, loaded through JNI so no `execve` is needed; a bind table, a synthesized `/proc` (`maps`, `mounts`, `mountinfo`, `loadavg`, `uptime`, `version`), fake ids, `--link2symlink`, and an interpreter and JIT that falls back to memfd dual mapping when SELinux restricts `execmem` | the bind table and the `/proc` synthesis in TODO, "The boundary" (step 3) and "Emulation"; its `--strace-full` decoder for debugging. It emulates the CPU too, so it is slower than deb-native by design (its own benchmark: about 270 for proot, 248 for its JIT, 94 for its interpreter, against 1164 native) |
| [OpceanAI/Doki](https://github.com/OpceanAI/Doki) | a container engine that picks one of twelve runners by probing the host, with proot as the Termux default and FEX-Emu, Box64 or QEMU for foreign architectures (most runners marked untested in its README) | a list of the probes a runner can make on Android (`/proc/sys/net`, SELinux denial of port 53); not a technique |
| [bun-on-termux](https://bun.termux.party/) | Bun on Termux with a C userland-exec wrapper and an `LD_PRELOAD` shim for `openat`, `stat`, `access`, `execve`, and `link`/`linkat`, which falls back to a copy where Android's f2fs refuses a hard link | the copy fallback is what `link2symlink` and the TODO, "Hard links that share contents", describe; its `/proc/stat` spoof for `os.cpus()` is an example of the emulation list |
| [termux/proot-distro issue #19](https://github.com/termux/proot-distro/issues/19) | proot gained System V IPC on a `sysvipc` branch: `msg*` and `sem*` implemented inside proot, `shm*` added later, behind `--sysvipc` (on by default in later git); the discussion says IPC is kept per proot instance | a native in-tracer answer for SysV, as an alternative to a broker; see "System V IPC and shared memory" |

## Patching `svc` on arm64

| project | how | what deb-native can take |
| --- | --- | --- |
| [retrage/svc-hook](https://github.com/retrage/svc-hook) | at load, every `svc` in the loaded code becomes a `b` to a trampoline (arm64 Linux and Android; Apache-2.0) | the shim patches only `mov x8,#nr; svc`; svc-hook's coverage reaches libc's generic `syscall()` and the loader's own calls (TODO, "Speed") |
| ASC-Hook ([arXiv 2412.05784](https://arxiv.org/abs/2412.05784)) | rewrites the pair (the load of `x8` and the `svc`), about 33 ns a call, signals as the fallback where a jump may land between the two | the open question it shares with the shim: is `x8` right at every jump target inside a patched pair |
| [google/gvisor#13362](https://github.com/google/gvisor/issues/13362) | an open issue on systrap for ARM64: the patch needs a register to clobber, and `x8` is the only one that is constant. The proposals: specialize a trampoline on the `x8` value (one per syscall number, under about 300), then on other registers by dynamic specialization, then a hash table keyed on `TPIDR_EL0` | the same constraint as the shim's pair. The proposal to specialize on the number is the answer to "is `x8` right here", at the cost of one trampoline per site; `TPIDR_EL0` is not set by Go, so the scheme would need a per-process off switch |

## Syscall User Dispatch

Linux 5.11 added Syscall User Dispatch (SUD): a thread that enables it gets a
`SIGSYS` for each syscall outside an allowed address range, and a selector byte
switches the filter on and off without a kernel call. It is not seen by ptrace
or the syscall tracepoints, and the setting does not survive `execve`.

| project | how | what deb-native can take |
| --- | --- | --- |
| [Syscall User Dispatch](https://docs.kernel.org/admin-guide/syscall-user-dispatch.html) (kernel docs; `prctl(PR_SET_SYSCALL_USER_DISPATCH)`) | the mechanism itself: a selector byte in user memory, an allowed range (typically the libc text), a signal per intercepted call | a third kind of net, below the tracer and beside the shim. It needs kernel 5.11 or later. Measured 2026-10-16 on the S10e (kernel 4.14.190): `prctl(PR_SET_SYSCALL_USER_DISPATCH, OFF)` returns `EINVAL`, so the kernel has no SUD. The Android 16 phone (S23 FE) runs kernel 5.10.240 ([device census](device-census.md)), which is also below 5.11: neither phone can use SUD, and a newer device with the Android 16 GKI kernel (6.12) would need a check of `CONFIG_SYSCALL_USER_DISPATCH`. The kernel's own docs say the calls are not visible to ptrace, so a tracer cannot count them |
| [lazypoline](https://github.com/lazypoline/lazypoline) (Jacobs et al., [DSN 2024](https://doi.org/10.1109/DSN58291.2024.00030), "System Call Interposition Without Compromise") | SUD as the slow path that finds every syscall instruction, and a rewrite of each one to call the interposer directly as the fast path (zpoline on x86-64). The SIGSYS handler rewrites the site, then resumes at the fast entry with the selector still set to ALLOW, so the handler's own syscalls are not intercepted | the same split as deb-native's shim (the fast path in the process) and tracer (the net): a rewrite of the site, and a catch-all for sites the rewrite missed. Its `dlmopen` bootstrap keeps the interposer's libraries apart from the program's, which is the answer to the shim's own `resolve` (TODO, "The law"). The paper's benchmarks are x86-64; arm64 is not confirmed |
| Secpoline (in lazypoline's README; USENIX'26 paper) | a follow-up adding interposer isolation against a malicious program | the selector byte sits in memory the program can write; the question it answers (who guards the net) applies to any shim that can be patched |

## Trap and patch elsewhere

| project | how | what deb-native can take |
| --- | --- | --- |
| gVisor's Systrap ([release notes](https://gvisor.dev/blog/2023/04/28/systrap-release/)) | seccomp `SECCOMP_RET_TRAP`, then the trapped `mov; syscall` rewritten to a jump so the next call skips the trap | the same "trap once, patch the site" shape; patching is x86 only, arm64 is an open issue (google/gvisor#13362, above) |
| zpoline, lazypoline, syscall_intercept | `syscall` rewritten to a call into a trampoline | x86-64 (and RISC-V for syscall_intercept); the ideas only |

## seccomp filters and their cost

| project | how | what deb-native can take |
| --- | --- | --- |
| [Constant-action bitmaps](https://lwn.net/Articles/834056/) (Kees Cook, YiFei Zhu; merged for Linux 5.11, [Phoronix](https://www.phoronix.com/news/Linux-5.11-SECCOMP-Performance)) | at filter attach time the kernel emulates each filter for every syscall; a syscall whose result reads only `arch` and `nr` gets a bit in an allow bitmap and skips the BPF run. Their benchmark: 29 ns of seccomp overhead for one bitmapped filter, 86 ns for three full filters | the filter's order, TODO "Speed". The shim's cookie in `args[5]` makes every syscall read an argument, so no bit is ever set. The fix there is to test `nr` first and the cookie only for the lane's numbers, as the TODO says; this reference shows why it is the whole game on 5.11+ kernels, and that the S10e (4.14) gets none of it |
| [Zero-consistency root emulation](https://arxiv.org/html/2405.06085v1) (Charliecloud, 2024) | compares seccomp-only "fake root" (no state, no emulated calls) with fakeroot and PRoot (state and emulation); the seccomp method is light, the stateful ones consistent | the trade-off behind TODO "Identity and files": the cheap fake root is the seccomp side, and the owner store is where it becomes consistent |

## Userland exec

Running an ELF without `execve`: the loader maps the program and its
interpreter in-process, and jumps in. Each project below is a different point on
the same road.

| project | how | what deb-native can take |
| --- | --- | --- |
| [ulexecve](https://github.com/anvilsecure/ulexecve) (Anvil Secure, Python; [blog](https://www.anvilsecure.com/blog/userland-execution-of-binaries-directly-from-python.html)) | parses the ELF, maps the `PT_LOAD` segments and the `PT_INTERP` loader, builds a jump buffer with the auxv fixed up (`AT_BASE`, `AT_PHDR`, `AT_ENTRY`); a `memfd` + `fexecve` fallback where that fails | the design of `dn-exec` in TODO, "A prefix in any app": a loader mapped into a process and entered directly, with auxv rebuilt |
| [userlandexec](https://github.com/bediger4000/userlandexec) (bediger4000, C) | a static userland `execve` for x86-64: maps `PT_LOAD`, maps `PT_INTERP` at a different base with `anywhere`, uses no libc | a small static loader as a model; it does not handle an interpreter's own libc state |
| [Santa's ELFs](https://www.rapid7.com/blog/post/2019/01/03/santas-elfs-running-linux-executables-without-execve/) (Rapid7, 2019) | `memfd_create` + `execveat` bypasses noexec, and the userland exec lineage (grugq); notes that Android's SELinux restricts the executable memory these need | the same lineage as ulexecve, and a warning that `PROT_EXEC` is the thing to test on the phone (TODO, "A prefix in any app") |
| `memfd_create` + `fexecve` ([Stack Overflow](https://stackoverflow.com/questions/63208333/using-memfd-create-and-fexecve-to-run-elf-from-memory), [unix.stackexchange](https://unix.stackexchange.com/questions/230472/can-i-exec-an-entirely-new-process-without-an-executable-file)) | an in-memory file exec'd by fd | measured 2026-10-16 on the S10e, in Termux's own domain (`untrusted_app_27`, targetSdk 28): `execveat(memfd, "", AT_EMPTY_PATH)` fails with `EACCES`, while the same ELF run by `execve` from a file in the app data directory runs. So a memfd is refused where a file is allowed. The domain of a targetSdk 29+ app is not measured (TODO, "A prefix in any app") |

## Exec on Android: W^X and the linker

| project | how | what deb-native can take |
| --- | --- | --- |
| [sepolicy commit 0dd738d8](https://android.googlesource.com/platform/system/sepolicy/+/0dd738d810532eb41ad8d90520156212ce756648) ("Enforce execve() restrictions for API > 28") and [the Android 10 behavior change](https://developer.android.com/about/versions/10/behavior-changes-10) | `execve` of files in the app's home is denied to `untrusted_app` from targetSdk 29; `mmap(PROT_EXEC)` and `dlopen` of such files stay allowed; `untrusted_app_25` and `untrusted_app_27` keep `execve`; `neverallow` rules in `app_neverallows.te` (Android 16 tree) keep the rule in place | the exact boundary behind TODO "A prefix in any app": `execve` refused, `dlopen` and `mmap` allowed. The sepolicy text is the place to check the next Android version |
| [termux-exec](https://github.com/termux-play-store/termux-exec) | an `LD_PRELOAD` wrapper on `execve` that runs `/system/bin/linker64 PATH` instead, so the kernel only sees the linker; it also rewrites `/bin` and `/usr/bin` shebangs. Its limits: `/proc/self/exe` and `/proc/*/comm` name the linker, statically linked binaries fail, and a raw `syscall` bypasses it | the trampoline TODO, "A prefix in any app", already has a working form. Termux's own fix for `/proc/self/exe` ([termux-packages#20767](https://github.com/termux/termux-packages/pull/20767)) is the same problem as the tracer's `exe_now` answer |
| [termux-app issue #1072](https://github.com/termux/termux-app/issues/1072) | the Termux thread on Android 10: `exec` from the native library directory (`/data/app/.../lib`) is allowed for targetSdk 29 but read-only; the maintainers say `dlopen` and `valgrind` on app-data files keep working, `execmem` is still used by the ART JIT, and the policy may later refuse `exec` altogether | why a runtime that runs prefixes needs a plan that does not depend on `execve` staying allowed |
| [Termux and Android 10](https://github.com/termux/termux-packages/wiki/Termux-and-Android-10) (wiki) | Termux stays at targetSdk 28 for the same reason TODO's "A prefix in any app" does | the same constraint, from the people who live with it |
| [Application Sandbox](https://source.android.com/docs/security/app-sandbox) (AOSP) | per-app SELinux domains from targetSdk 28, the limited raw filesystem view from Android 10 | the background for the narrowing of host directories (TODO, "The boundary", step 4) |

## Root emulation and path walks

| project | how | what deb-native can take |
| --- | --- | --- |
| [PRoot issue #124](https://github.com/proot-me/PRoot/issues/124) (2017) | proposes caching `lstat` by canonical path, and a trie that invalidates a subtree on rename or delete; measures `translate_syscall` at 47% of a `-b`-only run | the link and cwd cache in `dn-tiny` already caches by path; the invalidation question (a rename removes the cached subtree) is the one this thread names |
| [Landlock filesystem rule caching](https://kernsec.org/pipermail/linux-security-module-archive/2021-June/027663.html) (2021 patch series) and [landlock-lsm issue #9](https://github.com/landlock-lsm/linux/issues/9) | caches access checks on the `dirfd` of a path walk, so `find` skips the walk to the root; the cover letter says the cost goes from O(depth) toward O(1) for a walk through `dirfd`s, and measures it at depths 1 to 20 | the same idea for the tracer's walk through an `openat` `dirfd`; the patch series is not merged, so the numbers are the cover letter's |
| [Guix pack -RR](https://hpc.guix.info/blog/2020/05/faster-relocatable-packs-with-fakechroot/) and [eownerdead/proot-vs-fakechroot](https://codeberg.org/eownerdead/proot-vs-fakechroot) | fakechroot as a faster engine than proot for relocatable packs, with the same `ld.so` gap noted below; the benchmark is one tar extraction, a single run, with differences within the noise | a fakechroot-like path on the shim's libc side, for the calls the tracer does not need to see; the one-run benchmark is not evidence |

## Loader and libc interposition

| project | how | what deb-native can take |
| --- | --- | --- |
| [Guix fakechroot engine](https://hpc.guix.info/blog/2020/05/faster-relocatable-packs-with-fakechroot/) (2020) | `LD_PRELOAD` interposes libc's file calls, and the audit interface's `la_objsearch` hook rewrites where `ld.so` searches for libraries, since `ld.so`'s own `open` is not interposable | the answer to the loader's own calls in TODO, "Speed" (about 1,100 of the Lua build's 1,700 stops). `rtld-audit` is an option the exec gate could load for the `ld.so` search paths, not for every call |
| fakechroot (named in the eownerdead benchmark and in Charliecloud's paper) | `LD_PRELOAD` of the libc file calls only; it cannot wrap a static binary or a raw syscall, as the Charliecloud paper says | a model of what the shim's coverage excludes, and why the tracer exists |

## System V IPC and shared memory

| project | how | what deb-native can take |
| --- | --- | --- |
| [Winlator's glibc patches for SysV shm](https://deepwiki.com/brunodev85/winlator/7.1-glibc-patches-for-android) (DeepWiki, 2026) | `shmget`, `shmat`, `shmdt`, `shmctl` in glibc call a `sysvshm_server` over a unix socket; the server allocates the memory with Android's mechanisms and returns the fd by `SCM_RIGHTS`; `shmat` maps it with `MAP_SHARED` and caches the address per id | the broker design in TODO, "The boundary" and "Emulation" (SysV, a memfd per segment, a broker in the tracer that hands the fd over a unix socket); the protocol is a working example of it |
| [pelya/android-shmem](https://github.com/pelya/android-shmem) (2013) | SysV shm over ashmem through `LD_PRELOAD`, with `sendmsg`/`recvmsg` for the fds; used by Xserver and Linux GUI programs in chroots | the shm half of the SysV work, and why the TODO's SysV design needs `memfd`, not ashmem, on newer kernels |
| [lassik/shm_open_anon](https://github.com/lassik/shm_open_anon) | an anonymous shared-memory fd (`memfd_create`, or `shm_open` with the name removed) that can be passed over a unix socket with `SCM_RIGHTS` and survives `exec` if `FD_CLOEXEC` is cleared | the fd-passing rule the broker needs: clear `FD_CLOEXEC` only on the fd handed to a child, and give it a known number with `dup2` |
| [Adding System V IPC support via LD_PRELOAD](https://github.com/termux/proot-distro/issues/19) (proot-distro #19) | the proot maintainer adds `msg*` and `sem*` in proot, and `shm*` later; `libandroid-shmem` is what worked until then | the order the work went in (msg and sem before shm), for the TODO's SysV item |

## Other hosts, devices, and ABI

The projects below are not a syscall technique; they are platforms where a
host owns the screen, the devices, or the kernel personality.  They are
references for the device table ([`../spec/devices.md`](../spec/devices.md)),
the display backend, and the "hardware thật" route, not a dependency list.

### A host that owns the screen

| project | how | what deb-native can take |
| --- | --- | --- |
| **ChromeOS Crostini** (Termina VM, `crosvm`, **Sommelier**, Xwayland, virtio-gpu, 9p/virtiofs) | Linux runs in a VM; Sommelier is a Wayland/X proxy that presents guest windows on Chrome's compositor; Xwayland covers X11 clients | the split "guest compositor vs host compositor" and a proxy protocol; the host owns the display, the guest owns its desktop.  This is the closest mature shape to an app as display host, with a VM instead of a prefix |
| **WSLg** ([microsoft/wslg](https://github.com/microsoft/wslg)) | a system distro runs Weston; Windows presents it over RDP/VAIL; audio and clipboard are bridged; Xwayland for X11 | display/audio/clipboard transport as a host service; the client stays stock |
| **Android Terminal app / AVF** (`pKVM` + `crosvm`; Android 15 QPR2/16; [nixos-avf](https://github.com/nix-community/nixos-avf), [arch-arm64-avf](https://github.com/vitorpy/arch-arm64-avf)) | Android runs a Linux VM with virtio devices; the Terminal app is the front end; images are installed as a guest | the real-hardware route on Android: virtio, no root, a real guest kernel.  It is **VM-shaped and system-level**, not an app API; GPU/virtio-gpu support is device-dependent ([gfxstream](https://github.com/google/gfxstream)) |
| **Waydroid / Anbox / ARCVM / Halium** | the inverse: Android in a container/VM on Linux, with binder/gralloc adaptations | what a binder/gralloc boundary looks like, and why a container is not a prefix; not a path for Debian |

### Android hardware from glibc

| project | how | what deb-native can take |
| --- | --- | --- |
| [libhybris](https://github.com/libhybris/libhybris) | glibc programs load bionic HALs: gralloc, EGL, HWC, sensors | a possible real-GPU/display path without a DRM card; the app sandbox and HAL permissions decide whether it is usable |
| [NotKit/xf86-video-hwcomposer](https://github.com/NotKit/xf86-video-hwcomposer) | an Xorg DDX that renders through Android's HWComposer via libhybris | a worked "glibc + HWC" adapter; a data point for display without SurfaceFlinger |
| [google/gfxstream](https://github.com/google/gfxstream), virglrenderer, venus, ANGLE, Zink/Turnip | GPU virtualization or translation for a VM or a remote renderer | the GPU half of the VM route; not needed for the first synthetic devices |

### Userspace devices

| project | how | what deb-native can take |
| --- | --- | --- |
| [xspager/fuse_fb](https://github.com/xspager/fuse_fb) | CUSE implements a `/dev/fb0`-like char device in userspace | the ioctl/mmap shape of a synthetic framebuffer; CUSE needs a kernel module and root, so only the semantics are taken |
| [stefanberger/swtpm](https://github.com/stefanberger/swtpm), [OpenMandrivaSoftware/ossp](https://github.com/OpenMandrivaSoftware/ossp) | CUSE devices for TPM and OSS audio | examples of a char device whose ioctl answers come from a userspace model |
| [rust-vmm/vfio-user](https://github.com/rust-vmm/vfio-user), vhost-user | a device backend in another process, over a socket and shared memory | the provider/backend split and the fd-passing discipline for the device table |
| QEMU TCG with virtio-gpu/input (in Termux) | a full VMM in userspace, no KVM; virtio devices are real to the guest | the "true hardware" PoC on a phone, and a benchmark for why a prefix is cheaper |

### Input, seat, udev

| project | how | what deb-native can take |
| --- | --- | --- |
| [illiliti/libudev-zero](https://github.com/illiliti/libudev-zero) | a daemonless libudev replacement; reads sysfs and `/run/udev/data` | libinput without a udev daemon, if a synthetic sysfs view exists |
| [kennylevinsen/seatd](https://github.com/kennylevinsen/seatd) | a minimal seat management daemon (`libseat`) | a seat for a guest compositor without logind; useful if a virtual DRM model appears |
| mdevd / mdev | small device managers | a replacement for udevd in a tree; not needed for v1 |
| evemu, evtest, ydotool | event recording, reading, injection | conformance tools for evdev/uinput |

### ABI personalities and kernel-in-userspace

| project | how | what deb-native can take |
| --- | --- | --- |
| [darlinghq/darling](https://github.com/darlinghq/darling), [Wine](https://github.com/wine-mirror/wine) | a loader plus a personality layer (Darling also has a kernel module); Wine's `ntdll` is a userspace NT kernel | how a personality is separated from the host, and where a loader must sit; neither runs a Debian tree on Android |
| User-Mode Linux (`arch/um`) | the Linux kernel runs as a process; devices are host-backed | a real kernel without a hypervisor, but **x86 only**; not usable on arm64 |
| LKL (`lkl/linux`) | Linux kernel code as a library | a real kernel in-process; device support is host-specific, so it is research, not a drop-in |
| Drawbridge, Lupine, Gramine, OSv, Unikraft | library OS / unikernel: rebuild the program or libc instead of patching it | the other side of the bet; useful as a control, not as the runtime |

### Remote display and transport

| project | how | what deb-native can take |
| --- | --- | --- |
| SPICE, Looking Glass, wayvnc, Xpra | pixels and input over a protocol | data-plane designs for a display backend; not a reason to put a server in the architecture |
| Weston headless, Xvfb, Cage | a server/compositor without a physical output | reference behaviour for a virtual screen; the server route the device table avoids |
