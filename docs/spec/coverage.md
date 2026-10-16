# Coverage: every reader of a coordinate

Phase 1 of [`plan.md`](plan.md) maps each place a program can read a coordinate
(a path, an id, a descriptor, the mount view) to the tree's view.  This is the
checklist: one row per reader, so a hole is visible.  "Done" means the tracer
(and the shim where it applies) answers the tree's value; the test is per row.

The idea of the phase: a value is mapped only when **every** reader of it is
mapped, so a program cannot find the host frame through an unmapped reader.

## Path in

| reader | state |
| --- | --- |
| every syscall with a path argument (`gen_rows.py`, the rows) | done |

## Path out (the kernel naming a path)

| reader | state |
| --- | --- |
| `getcwd` | done (`fix_cwd`) |
| `readlink`/`readlinkat` targets | done (`fix_link`) |
| `/proc/<pid>/cwd` | done (`readlinkat`) |
| `/proc/<pid>/exe` | done (`exe_now`) |
| `/proc/<pid>/fd/<n>` (a link to its target) | done (`readlinkat`) |
| `/proc/<pid>/mounts`, `mountinfo`, `/etc/mtab` | done (`B_SYNTH` rows) |
| `/proc/<pid>/maps`, `/proc/<pid>/smaps` (file-backed lines) | **open**: the content names host paths |
| `getdents64` names | leaf names, fine; a host row lists the host's (intended) |
| `statx` | no name returned (nothing to map) |

## fds

| reader | state |
| --- | --- |
| `/proc/<pid>/fd/<n>` | done |
| `fstat`/`statx` owner | done (fake root) |
| `fdinfo` | no path |

## AF_UNIX

| reader | state |
| --- | --- |
| `bind`, `connect`, `sendto`, `sendmsg` (the path in) | done (`SP_SOCK_IN`) |
| `getsockname`, `getpeername`, `accept`, `recvfrom`, `recvmsg` (back) | done (`SP_SOCK_OUT`) |

## The mount view

| reader | state |
| --- | --- |
| `mounts`, `mountinfo`, `/etc/mtab` | done |
| `maps`, `smaps` | **open** (see path out) |

## Other host surfaces

| reader | state |
| --- | --- |
| `/sys`, `/dev`, `/storage` | host rows (bound as they are; a later step narrows) |
| `openat2` with `RESOLVE_*` | **open**: answers `ENOSYS` |

## The test

A small program that walks each surface and prints the tree's view, and a script
that fails when the output holds a host path (or a host id, or the host's mount
table).  To be built, run interleaved with the regression set and the bench.
