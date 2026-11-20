# Kernel mechanisms the runtime can lean on

[FOA's kernel mechanisms](https://github.com/jronminh/fhs-on-android/blob/main/reference/mechanisms.md) describes
the portable facilities. The tables below record the device matrix used to
choose mechanisms for deb-native as of 2026-10-16. The 4.14 phone is the
S10e (Android 12) and the 5.10 phone is the S23 FE (Android 16).
See the [device census](device-census.md) for context.

## Interception

Both reference kernels support the `ptrace` and seccomp trace path used
by `dn-init`. The two kernels differ in notification support, which
limits where a userspace supervisor could supply fds without changing
the prefix's path model.
[FOA interception matrix](https://github.com/jronminh/fhs-on-android/blob/main/reference/mechanisms.md#interception-for-a-runtime)

`PTRACE_SEIZE` and `PTRACE_INTERRUPT` are available on both reference
kernels and would give the tracer a stop independent of a signal.

## The tracer's loop

| mechanism | since | 4.14 | 5.10 | use to us |
| --- | --- | --- | --- | --- |
| `pidfd_open` + `waitid(P_PIDFD)` | 5.3 / 5.4 | no | yes | a handle to a tracee that pid reuse cannot steal |
| `eventfd` / `signalfd` / `epoll` | old | yes | yes | wait on the tracee and its fds at once |
| `PTRACE_O_EXITKILL` | 3.8 | yes | yes | kill the tracee if the tracer dies |
| `PTRACE_O_TRACESECCOMP` / `PTRACE_O_TRACEEXEC` | 3.8 | yes | yes | the `SIGSYS` and exec events already used |

## Reading and writing a tracee

| mechanism | since | 4.14 | 5.10 | use to us |
| --- | --- | --- | --- | --- |
| `process_vm_readv` / `writev` | 3.2 | yes | yes | a tracee's memory without a `ptrace` PEEK/POKE per word (the registers still need `ptrace`); worth it for a `stat` buffer or a `sockaddr` |
| `PTRACE_PEEK` / `POKE` (today) | old | yes | yes | small reads (a path) |
| `membarrier` | 4.3 | yes | yes | a cheap barrier across threads, if a seqlock ever needs one |

## Shared memory and files

FOA records the `memfd` and reflink support in its
[shared-memory reference](https://github.com/jronminh/fhs-on-android/blob/main/reference/mechanisms.md#shared-memory).
For deb-native, a tracee could receive a `memfd` over a socket.
Opening it through the tracer's `/proc/<tracerpid>/fd/N` returned `EACCES`.

| mechanism | since | 4.14 | 5.10 | use to deb-native |
| --- | --- | --- | --- | --- |
| `SECCOMP_IOCTL_NOTIF_ADDFD` | 5.9 | no | yes | Supply a synthesized `/proc` file as an fd without storing it under `var/lib/deb-native` |
| `dma_heap` / `udmabuf` | 5.6 / 4.20 | no / yes | yes | Back graphics buffers |
| `FICLONE` | 4.5 | to check | to check | Reflink fallback for the hard-link copy |

## Caching: know when a cached answer moved

| mechanism | since | 4.14 | 5.10 | use to us |
| --- | --- | --- | --- | --- |
| `inotify` | 2.6.13 | yes | yes | watch the tree and drop the link cache on a real change, instead of the coarse shared generation (`var/lib/deb-native/dn-tiny.gen`) |
| `fanotify` | 2.6.36 (fs marks 5.1) | partial | yes | heavier; not needed |
| vDSO (`clock_gettime`, `getcpu`) | old | yes | yes | a fast path the kernel itself provides -- no trap; the baseline a fast path can never beat |

## Device limits

The older 4.14 device limits the tracer design to mechanisms available on
that kernel, including `ptrace` and `RET_TRAP`. Device policy can further
limit a mechanism exposed by the kernel. [FOA syscall reference](https://github.com/jronminh/fhs-on-android/blob/main/reference/syscalls.md)

## What is worth trying (in order)

1. **seccomp user notification + `ADDFD`**, on a 5.10 phone: catch the calls the
   shim cannot reach without a `ptrace` wake, and hand back an fd instead of
   writing a file and pointing the open at it. It is a *supervisor*, not a
   sandbox -- the man page is explicit that it is not a security policy, that a
   `CONTINUE` response is a TOCTOU race, and that every read of the target's
   memory needs a `SECCOMP_IOCTL_NOTIF_ID_VALID` check (`seccomp_unotify(2)`).
2. **`pidfd` + `epoll`** in the tracer's loop: a race-free wait (no pid reuse),
   one poll for the tracee and its fds.
3. **`process_vm_readv`/`writev`** for the big copies (a `stat`, a `sockaddr`).
4. **`inotify`**: a precise link-cache drop instead of the whole-cache
   generation -- fewer false drops, and no shared file to keep coherent.
5. **vDSO**: confirm glibc already takes it (time, `getcpu`), so those calls
   never reach the tracer at all.

None of these changes the *mapping*; each is an engine under it, chosen so that
the platform will not kill it.
