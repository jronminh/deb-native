# Kernel mechanisms the runtime can lean on

Which of the kernel's own facilities help the runtime, and which Android's
policy takes away. The kernel is the engine the mapping runs on
([`../architecture.md`](../architecture.md)); by rule 7 a mechanism is a
*stable* foundation ([`../spec/mapped-kernel.md`](../spec/mapped-kernel.md)) only
when the kernel provides it **and** the app's filter lets it through. The first
column is the kernel version it needs; the next two are the two phones -- the
S10e (Android 12, kernel 4.14) and the S23 FE (Android 16, kernel 5.10,
[`device-census.md`](device-census.md)). Checked 2026-10-16; "to check" means
not measured on this phone.

## Interception: how a call is caught

| mechanism | since | 4.14 | 5.10 | use to us |
| --- | --- | --- | --- | --- |
| seccomp `RET_TRACE` + `ptrace` (today) | 3.5 / 3.8 | yes | yes | the tracer, the correctness net |
| seccomp `RET_TRAP` + a `SIGSYS` handler in-process | 3.5 | yes | yes | the in-process fast path (E12/E13); the handler must survive `exec` |
| **seccomp user notification** (`SECCOMP_RET_USER_NOTIF`) | 5.0 | no | yes | the kernel's own supervisor model (systemd, Chrome, gVisor): a listener fd, no `ptrace`; `SECCOMP_IOCTL_NOTIF_ADDFD` (5.9) hands the tracee an **fd**, so a synthesized file can be a `memfd`, not a file on disk |
| `PTRACE_SEIZE` / `PTRACE_INTERRUPT` | 3.8 / 3.10 | yes | yes | a stop that is not a signal |
| Syscall User Dispatch | 5.11 | no | no | a third net below the tracer; neither phone has it |

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

| mechanism | since | 4.14 | 5.10 | use to us |
| --- | --- | --- | --- | --- |
| `memfd_create` | 3.17 | yes | yes | the SysV broker; a file to hand over a unix socket (but a tracee cannot open the tracer's `/proc/<tracerpid>/fd/N`: `EACCES`) |
| `SECCOMP_IOCTL_NOTIF_ADDFD` | 5.9 | no | yes | with user notification, put an fd into the tracee -- the mount view and `status`/`cmdline` need no file under `var/lib/deb-native` |
| `dma_heap` / `udmabuf` | 5.6 / 4.20 | no / yes | yes | GPU buffers (TODO, "Graphics") |
| `FICLONE` | 4.5 | to check | to check | a real reflink for the hard-link copy, where the filesystem has it |

## Caching: know when a cached answer moved

| mechanism | since | 4.14 | 5.10 | use to us |
| --- | --- | --- | --- | --- |
| `inotify` | 2.6.13 | yes | yes | watch the tree and drop the link cache on a real change, instead of the coarse shared generation (`var/lib/deb-native/dn-tiny.gen`) |
| `fanotify` | 2.6.36 (fs marks 5.1) | partial | yes | heavier; not needed |
| vDSO (`clock_gettime`, `getcpu`) | old | yes | yes | a fast path the kernel itself provides -- no trap; the baseline a fast path can never beat |

## What Android takes away

From the census: `rseq` and `set_robust_list` are killed on **both** phones;
`clone3` and `faccessat2` on 4.14 only (allowed on 5.10); `io_uring`, `bpf`, the
new mount API and the xattr-at calls on 5.10. So a mechanism is usable only
where the kernel **has** it and the filter **allows** it. That is why the
interesting ones here are 5.0+/5.9+ and, today, the S23 FE only: on the S10e
(4.14) the runtime has no choice but `ptrace` and `RET_TRAP`.

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
