# POSIX IPC: shared memory and message queues

Status: **mixed** (2026-10-19).  POSIX shm is **implemented** in 0.9.0: the law
maps `/dev/shm` to `<root>/run/shm`, and `init.sh` creates the directory on
both hosts.  POSIX message queues are a **proposal**: `mq_open`, `mq_send`,
`mq_receive` and `mq_notify` are seccomp-killed and the kernel config is off,
like SysV.

The first client for shm is a Wayland compositor's `wl_shm`; the first for
mqueue is a small C test (and, later, a service that uses `mq_notify`).

## POSIX shared memory

### The directory

The law maps `/dev/shm` to `<root>/run/shm`; `init.sh` creates it mode `1777`
on both hosts.  `/dev` stays the host row, and the mapping is part of the
path law, not a separate broker.

- Names map to regular files under the tree directory; glibc does the `/`
  handling.  `shm_open` with `O_CREAT|O_EXCL`, `O_RDWR`, mode, then
  `ftruncate` and `mmap`, is the usual path.
- `shm_unlink` removes the name; mappings keep the pages alive until unmapped.
  A regular filesystem gives the same unlink semantics.
- `fstat`, `fchmod`, `fchown` follow the normal file path (the owner store).
  `st_dev` is the tree's, not a tmpfs; programs that insist on a tmpfs are
  outside the promise.
- The directory lives with the tree, so a name survives until it is unlinked or
  the app data is cleared; it is not a per-boot tmpfs.  That difference is
  documented.

### Why not a tmpfs

An app cannot mount a tmpfs without privilege.  A directory under the prefix is
the honest substitute; `statfs` reports the tree's filesystem.

### Tests

- `shm_open` + `ftruncate` + `mmap` in two processes; `shm_unlink` while
  mapped; `O_EXCL`; bad names; mode checks.
- Python `multiprocessing.shared_memory`.
- A Wayland client using `wl_shm` (the real target).

## POSIX message queues

### Model

A **broker** owns queues, their messages and their waiters; `mq_open` returns an
fd that the broker backs.  Messages are copied; queues are small.  The broker
may live in the runtime or in a small per-prefix daemon (see
[`ipc-sysv.md`](ipc-sysv.md), "Open questions").

| call | behaviour |
| --- | --- |
| `mq_open(name, flags, mode, attr)` | create/look up; `O_CREAT`/`O_EXCL`; returns an fd; `attr` sets `mq_maxmsg`/`mq_msgsize` |
| `mq_unlink(name)` | remove the name; open descriptors keep the queue until closed |
| `mq_send`/`mq_timedsend` | enqueue by priority; block when full; `EAGAIN` on timeout |
| `mq_receive`/`mq_timedreceive` | highest priority first, FIFO within a priority; block when empty; truncation is `EMSGSIZE` |
| `mq_getattr`/`mq_setattr` | current flags and limits; `O_NONBLOCK` |
| `mq_notify` | one registration per queue; `SIGEV_SIGNAL` to the process; `SIGEV_NONE`; `EBUSY` if already registered |
| `close` | drop the descriptor; the queue lives until unlinked and closed |

- Names are per prefix; the broker strips the leading `/` glibc may pass.
- `mq_notify` with `SIGEV_THREAD` is glibc's own emulation over a signal; the
  broker only has to deliver `SIGEV_SIGNAL` and let glibc's helper thread run.
- A signal must not be lost: the broker tracks "one notification armed", sends
  it when the queue becomes non-empty, and clears the registration.
- Limits: `mq_maxmsg`, `mq_msgsize`, `msg_max`, `msgsize_max`, `queues_max`
  are virtual; `/proc/sys/fs/mqueue/*` is answered from them if tools read it.
- `/dev/mqueue` (the mqueue filesystem view) is not promised in v1; `mq_open`
  by name works, listing does not.

### Tests

- A C test: send/receive priorities, blocking, timed out, `O_NONBLOCK`,
  `mq_notify` with a signal, unlink while open.
- Two processes, one queue; a process exit while waiting is `EINTR`/`EIDRM`.

## Shared limits and security

- Both are per prefix; no sharing with Android or another prefix.  POSIX shm
  names live with the tree; message queues would be per boot.
- Permissions are emulated; the host authority is untouched.
- The broker enforces queue and message limits, and the shm directory is
  bounded by the tree's filesystem; a guest cannot exhaust the app's memory
  through either without hitting a documented limit.

## Milestones

| | scope |
| --- | --- |
| **P1** | done: the `/dev/shm` mapping + regular-file shm; `shm_open`/`unlink`; `wl_shm` test |
| **P2** | mqueue broker: open/send/receive/unlink/attrs |
| **P3** | `mq_notify` signals; `/proc/sys/fs/mqueue` view |

P1 is the path mapping in `policy.c` plus the directory from `init.sh`; it needs
no broker.  P2 needs the same kind of stateful service as SysV `msg`, and can
share its transport.

## Non-goals

- A tmpfs (no privilege to mount one).
- `/dev/mqueue` directory listing.
- A per-boot tmpfs.  Shm names live with the tree until unlinked.
- Kernel-enforced permissions.

## Open questions

1. **Stale shm names**: `init.sh` creates `<root>/run/shm` but does not clear
   it; POSIX shm is per boot.  Should boot remove old names, and how without
   touching a user's own files?
2. **`statfs`**: report the tree's filesystem, or synthesize a small tmpfs-like
   answer?  The former is honest; the latter may satisfy a probe.
3. **Broker sharing**: one daemon for SysV and POSIX queues, or two?
4. **`mq_notify` race**: the exact rule for a queue that becomes non-empty
   between `mq_notify` and the signal.
5. **`/dev/mqueue`**: is it worth synthesizing for `ls`/`ipcs`-style tools?

## Sources

glibc's `shm_open`/`mq_*` man pages and the Linux `mqueue`/tmpfs
documentation; the `/dev/shm` mapping in `src/dn-tiny/policy.c` and the directory
in `scripts/prefix/init.sh`; `lassik/shm_open_anon` for fd-passing; the SysV
broker design in [`ipc-sysv.md`](ipc-sysv.md) for the queue state.  The shm
target is a Wayland `wl_shm` client; the queue target is a `mq_notify` test.
Gramine ([`prior-art.md`](../reference/prior-art.md), "System V IPC and
shared memory") does not implement `mq_*` either, and its own POSIX shm is
restricted to accelerator use cases -- its shm restriction matches the P1
choice here, but mqueue (P2/P3) has no library-OS design to borrow from it.
