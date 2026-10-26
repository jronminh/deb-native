# Coverage: every reader of a coordinate

Phase 1 ([`mapped-kernel.md`](mapped-kernel.md), "What this orders") maps each
place a program can read a coordinate (a path, an id, a descriptor, the mount
view) to the tree's view.  This is the checklist: one row per reader, so a
hole is visible.  "Done" means the tracer (and the shim where it applies)
answers the tree's value; the test is per row.

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
| `/proc/<pid>/maps`, `/proc/<pid>/smaps` (file-backed lines) | done (`synth_maps`) |
| `getdents64` names | leaf names, fine; a host row lists the host's (intended) |
| `statx` | no name returned (nothing to map) |

## fds

| reader | state |
| --- | --- |
| `/proc/<pid>/fd/<n>` | done |
| `fstat`/`statx` owner | done (fake root) |
| `fgetxattr`/`flistxattr` (the fd's file, for the SELinux label) | done |
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
| `maps`, `smaps` | done (see path out) |

## The process view

| reader | state |
| --- | --- |
| `/proc/<pid>/status`: `Name`, `State` | done (`synth_status`) |
| `/proc/<pid>/status`: `TracerPid`, `Seccomp` (the runtime's traces) | done (`synth_status`) |
| `/proc/<pid>/status`: `Uid`, `Gid` (fake root) | done (`synth_status`) |
| `/proc/<pid>/status`: `CapPrm/Eff/Bnd` (root's set, as the fake id is root) | done (`synth_status`) |
| `/proc/<pid>/cmdline` (the loader the gate adds, and its host paths) | done (`synth_cmdline`) |
| `/proc/<pid>/comm` (the loader, not the program) | done (`synth_comm`) |
| `PPid`, `getppid()` | the tracer: a structural residue (`phase2.md`, "An accepted residue") |

## Other host surfaces

| reader | state |
| --- | --- |
| `/proc`, `/sys`, `/dev` | host rows (the kernel's own) |
| `/data`, `/storage`, `/sdcard`, `/mnt`, `/system`, `/apex`, `/vendor` | the tree's now (`ENOENT`; a bind reaches one on purpose) |
| `openat2` with `RESOLVE_*` | refused `ENOSYS` (a clear refusal; without a resolve flag it runs as `openat`, the path mapped) |

## The test

`scripts/measure/coverage.sh PREFIX` copies `scripts/measure/coverage-probe.sh` into the prefix,
runs it (it prints the tree's view of each surface above), and fails on any line
that holds the host frame -- the prefix's own path, `/data/data`, the shared
storage, the tracer.  Run it interleaved with the regression set and the bench.

It comes back **clean**: the environment and the working directory since "The
boundary", step 1; the SELinux label since step 2; the mount view, `maps`/`smaps`,
the process view (`cmdline`, `comm`, and `status`'s `Uid`/`Gid`/`Name`/`TracerPid`)
and every other reader of the checklist above, mapped or generated.
