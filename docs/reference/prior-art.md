# Prior art: who else answers Linux one syscall at a time

Projects that do a part of what deb-native does, and what each one offers it.
None of them combines the three parts deb-native rests on
([`architecture.md`](../architecture.md)): an unmodified Debian glibc tree,
a shim that patches the `svc` sites in the process, and a tracer as the net
under it, both answering by one law. Checked 2026-10-15.

## Contents

- [The closest relative: WSL1](#the-closest-relative-wsl1)
- [On Android](#on-android)
- [Patching `svc` on arm64](#patching-svc-on-arm64)
- [Trap and patch elsewhere](#trap-and-patch-elsewhere)

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
| [termux-pacman/glibc-packages](https://github.com/termux-pacman/glibc-packages) | a glibc rebuilt so it never makes the calls Android kills: `gpkg/glibc/fakesyscall.json` maps each one to its answer (`faccessat2` → `faccessat`, `statx` → a generic `statx`, the `set*id` family → 0, `shm*` → an ashmem emulation) | the table, checked on Android, as a source for the rows (TODO, "Rows from a catalog"); no path translation |
| [pelya/debian-noroot](https://github.com/pelya/debian-noroot) | PRoot, `fakeroot-tcp` behind `sudo`, a preload stubbing libselinux and xattrs and emulating SysV shm over ashmem (`android-shmem`) | the environment it keeps, a `passwd` line for the app's UID; its 2012 fakechroot attempt (preload only) failed, as a preload alone does |
| [huntergdavis/steamclienttermux](https://github.com/huntergdavis/steamclienttermux) | Steam on Termux glibc outside PRoot, games under a patched PRoot | robust lists and SysV semaphores with real semantics are what Steam needs |

## Patching `svc` on arm64

| project | how | what deb-native can take |
| --- | --- | --- |
| [retrage/svc-hook](https://github.com/retrage/svc-hook) | at load, every `svc` in the loaded code becomes a `b` to a trampoline (arm64 Linux and Android; Apache-2.0) | the shim patches only `mov x8,#nr; svc`; svc-hook's coverage reaches libc's generic `syscall()` and the loader's own calls (TODO, "Speed") |
| ASC-Hook ([arXiv 2412.05784](https://arxiv.org/abs/2412.05784)) | rewrites the pair (the load of `x8` and the `svc`), about 33 ns a call, signals as the fallback where a jump may land between the two | the open question it shares with the shim: is `x8` right at every jump target inside a patched pair |

## Trap and patch elsewhere

| project | how | what deb-native can take |
| --- | --- | --- |
| gVisor's Systrap ([release notes](https://gvisor.dev/blog/2023/04/28/systrap-release/)) | seccomp `SECCOMP_RET_TRAP`, then the trapped `mov; syscall` rewritten to a jump so the next call skips the trap | the same "trap once, patch the site" shape; patching is x86 only, arm64 is an open issue (google/gvisor#13362: branch range, which register to use) |
| zpoline, lazypoline, syscall_intercept | `syscall` rewritten to a call into a trampoline | x86-64 (and RISC-V for syscall_intercept); the ideas only |
