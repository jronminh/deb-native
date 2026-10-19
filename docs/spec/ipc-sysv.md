# SysV IPC: shm, sem, msg

Status: **proposal** (2026-10-19).  T2 service emulation, not in 0.9.0.  The
host has neither the syscalls nor the kernel config: `shmget`/`semget`/`msgget`
and friends are killed by seccomp and `CONFIG_SYSVIPC` is off, so there is no
`/proc/sysvipc` and no kernel object to bind.  A user-space broker is the only
way to provide them.

Named workloads: X/Steam (MIT-SHM), PostgreSQL (a small shared segment),
and `ipcs`/`ipcrm`-style tools.  The contract rows are in
[`contract.md`](contract.md), "IPC, sockets, network".

## Model

One **broker per prefix session**, owned by the host side of the runtime.  It
holds every object the kernel would hold: keys, ids, permissions, sizes,
attach counts, semaphore values and waiters, message queues.  Clients talk to
it over a UNIX socket; a shared segment is a `memfd` whose fd is passed over
that socket with `SCM_RIGHTS`.

```
guest shmget/semget/msgget
   │  (seccomp-killed syscall, answered by the tracer/shim)
runtime IPC frontend
   │  broker protocol (key, id, op) + fd passing
broker (in the runtime or a small per-prefix daemon)
```

- The broker is **not** a kernel object: it is per boot, dies with the prefix,
  and is never shared with Android.
- Keys and ids are virtual and local to the prefix.  `IPC_PRIVATE` allocates a
  new id; a key is resolved in the broker's table.
- Permissions (`mode`, `uid`, `gid`) are the fake identity's; they grant
  nothing over the host.  They are enforced by the broker so that a program
  sees the same `EACCES` a Debian kernel would give.
- One prefix, one broker: two prefixes do not share segments, semaphores or
  queues.

## Shared memory

| call | behaviour |
| --- | --- |
| `shmget(key, size, flags)` | create or look up; `IPC_CREAT`/`IPC_EXCL`; `size` rounded to a page; limits from a virtual `shmmax` |
| `shmat(id, addr, flags)` | map the segment's `memfd`; `SHM_RDONLY`; `SHM_REMAP`; alignment is the page size |
| `shmdt(addr)` | unmap, decrement `nattch` |
| `shmctl(IPC_STAT/IPC_SET/IPC_RMID)` | answer from the broker; `IPC_RMID` marks the segment for deletion after the last detach (the kernel's rule) |

- The `memfd` is created by the broker and passed to the first attacher; later
  attachers receive the same fd.  `shmat` maps it; `shmdt` unmaps.
- `SHM_EXEC` is not promised: `memfd` exec is restricted in the app domain
  ([`contract.md`](contract.md)); a program that needs executable shared memory
  is refused.
- `shmctl(IPC_INFO/SHM_INFO)` is answered from virtual limits, or `EINVAL` if
  the fields cannot be made honest.
- `/proc/sysvipc/shm` is synthesized from the broker for tools, or absent in
  v1 (open question); `ipcs` may need it.

## Semaphores

| call | behaviour |
| --- | --- |
| `semget(key, nsems, flags)` | create/look up a set; `nsems` fixed |
| `semop(semid, sops, n)` | apply the array atomically; block if a value would go negative; `SEM_UNDO`; `IPC_NOWAIT`; `EINTR`/`EIDRM` |
| `semtimedop` | the same with a timeout (`EAGAIN` on expiry) |
| `semctl(GETVAL/SETVAL/GETALL/SETALL/GETPID/GETNCNT/GETZCNT/IPC_STAT/IPC_SET/IPC_RMID)` | answer from the set; `IPC_RMID` wakes waiters with `EIDRM` |

- Blocking is real: a waiter sleeps on an eventfd/futex until another process
  changes the set, another call removes it, or the timeout expires.  A busy
  poll is not an implementation.
- `SEM_UNDO` adjustments are per process; the broker applies them on exit and
  wakes waiters.  A process that dies without a clean exit is the hard case;
  the runtime's exit hook is part of the contract.
- `GETNCNT`/`GETZCNT` are exact only if the broker tracks waiters; if it does
  not, it must not guess (refuse or document a best-effort value).

## Message queues

| call | behaviour |
| --- | --- |
| `msgget(key, flags)` | create/look up |
| `msgsnd(qid, msgp, size, flags)` | enqueue by type; block when full unless `IPC_NOWAIT` |
| `msgrcv(qid, msgp, size, type, flags)` | type 0 = first, >0 = exact, <0 = first `<= abs(type)`; `MSG_NOERROR`, `MSG_EXCEPT`, `IPC_NOWAIT` |
| `msgctl(IPC_STAT/IPC_SET/IPC_RMID)` | answer from the queue; `IPC_RMID` wakes waiters with `EIDRM` |

- Queue limits (`msgmax`, `msgmnb`, `msgmni`) are virtual and reported
  consistently.
- Messages are copied, not shared: a queue is small; a ring in the broker is
  enough.

## Limits and views

- Virtual limits (one set per prefix): `shmmax`, `shmall`, `shmmni`, `semmsl`,
  `semmns`, `semopm`, `semmni`, `msgmax`, `msgmnb`, `msgmni`.  They are
  answered in `/proc/sys/kernel/*` where Debian tools read them.
- `/proc/sysvipc/{shm,sem,msg}` and `ipcs` are a **view** of the broker.  If the
  view is not implemented in v1, `ipcs` fails cleanly and the spec says so.
- `IPC_STAT` timestamps use the prefix's clock; they are not the host's.

## Lifetime

- Objects live until `IPC_RMID` or the prefix ends.  This mirrors SysV's
  "persist until removed" rule, bounded by one boot.
- The broker is started by the host before the first client and stopped with the
  session.  A crash loses all objects; a new boot starts empty.
- A segment's `memfd` keeps the pages alive while any process has it mapped,
  even if the broker is gone; the next boot has no record of it.

## Security

- The broker is a data service: it cannot grant host capabilities, open a host
  path, or reach Android.
- Permission checks are emulated, not kernel-enforced; a program that ignores
  them can still read its own process memory, but cannot reach the host.
- Limits are enforced by the broker so a guest cannot exhaust the app's memory
  or file descriptors.

## Tests and workloads

- A C test: create/attach/detach/remove a segment between two processes; check
  `nattch` and `IPC_RMID` semantics.
- Semaphores: two processes increment/decrement with blocking; `SEM_UNDO` on
  exit; a timeout; `IPC_RMID` waking a waiter.
- Messages: type selection, blocking, `MSG_NOERROR`, `IPC_NOWAIT`.
- Xvfb with MIT-SHM (the first real target), PostgreSQL's postmaster, Python's
  `multiprocessing` shared memory/semaphores.
- `ipcs`/`ipcrm` if the `/proc` view is in.

## Milestones

| | scope |
| --- | --- |
| **S1** | `shm` only (memfd + fd passing), enough for MIT-SHM/Xvfb |
| **S2** | `sem` with blocking and `SEM_UNDO` (Steam/PostgreSQL) |
| **S3** | `msg`; `/proc/sysvipc` view; limits |

S1 does not need a daemon: the tracer/shim can hold the key table and a
`memfd` per segment, with the fd passed over an internal socket.  S2 is where a
stateful broker earns its place.

## Non-goals

- Sharing objects with Android or another prefix.
- Kernel-enforced permissions or namespaces.
- `SHM_EXEC` (refused).
- 32-bit compat layouts.

## Open questions

1. **Broker location**: in the tracer (no extra process, but state in a
   syscall loop) or a small daemon (cleaner waits, one socket)?
2. **`SEM_UNDO` on a violent exit**: the runtime's exit hook must cover
   `SIGKILL`; if it cannot, the spec must document the residue.
3. **`/proc/sysvipc`**: synthesize it, or make `ipcs` fail honestly?
4. **Limits**: one virtual set, or follow the host's `/proc` where readable?
5. **Timestamps**: the prefix's clock vs the broker's monotonic time.

## Sources

[`../reference/prior-art.md`](../reference/prior-art.md), "System V IPC and
shared memory": Winlator's `sysvshm_server` (socket + `SCM_RIGHTS`),
`pelya/android-shmem` (ashmem; the X/MIT-SHM case), `lassik/shm_open_anon`
(the fd-passing rule), and `termux/proot-distro#19` (the msg/sem-before-shm
order).  The kernel semantics are the `shm*`/`sem*`/`msg*` man pages; the
workloads are Xvfb with MIT-SHM, PostgreSQL, and Steam.
