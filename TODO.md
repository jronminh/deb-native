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
- **Another phone**, and Termux from F-Droid as a user gets it.
- **dn-shell**: it runs `boot/boot.sh` of a prefix under `prefixes/`; its spec
  still describes `.dn/contract`.
- **A warning during the second stage**: Android's linker prints "failed to find
  generated linker configuration" once; find which program it is.
- **The default for `-s`**: on in `build-prefix.sh`, off in `boot.sh`. Measure
  `make -j` (the polling tracer competes for a core) and an app session before
  choosing a default for `boot.sh`.

## The law

- **One copy of the law**: the shim has its own resolver (`resolve`,
  `is_host_path` in `lane.c`); link `policy.c` into it instead. It needs a build
  of `policy.c` against glibc (its `sys.h` defines `_start` and the string
  functions) and its link cache made thread-safe.
- **Not mapped yet**: `execveat` (glibc's `fexecve`), `openat2` with
  `RESOLVE_*` (answers `ENOSYS`), `name_to_handle_at`, AF_UNIX socket paths
  (`bind`, `connect`, `sendto`, `sendmsg`; `getsockname`, `getpeername`,
  `accept`, `recvfrom`, `recvmsg` back), so a guest `bind("/tmp/x.sock")` reaches
  the host's `/tmp`: tmux, ssh-agent, gpg-agent, dbus need it.
- **Rows from a catalog**: generate the rows from one table giving each syscall
  a class (path, result, socket, exec, identity, refused, none) instead of the
  tables in `gen_rows.py`.

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

## Speed

- **`ld.so`'s own calls** (about 1,100 of the Lua build's 1,700 stops): the
  program, the shim and the libraries are opened before the shim is loaded. The
  exec gate could patch the loader's `svc` sites as the shim does libc's.
- **libc's cancellable path** (324): its `svc` is left alone for
  `pthread_cancel`; a stub that keeps the program counter inside
  `__syscall_cancel_arch`'s range would allow it.
- **Who sets the floor of a stop**: on this phone the tracer's big core sits at a
  low clock while the guest runs on the little cluster. Try the tracer and the
  guest on one cluster, a priority raised with `nice` (no privilege needed down to
  -20), an ADPF hint session from the app.

## Emulation

Answer what Android refuses in place of the kernel, the same in the tracer and
the shim:

- a synthetic `/proc` and `/sys` (`/proc/self/mounts`, `/proc/version`,
  `/proc/sys/*`), generated at `open` into a memfd so `read` needs no stop;
- a virtual mount table (`mount`, `umount2`, `statfs`, `/proc/self/mountinfo`);
- privileged ports (a bind translation, and `getsockname`/`accept` back);
- capabilities (`capget`, `PR_CAPBSET_READ`);
- a clear refusal (`EPERM` or `ENOSYS`, with the reason) for what cannot be
  faked: real isolation, TUN, anything an LSM sees.

## Building a prefix

- `build-prefix.sh` relies on Termux's `debootstrap` for the first stage, which
  unpacks through `proot --link2symlink`. A first stage of its own (resolve the
  base set, fetch, unpack with hard links as copies) would drop proot from the
  build.
- Suites other than Debian's (Ubuntu, Kali) and `--variant` other than `minbase`
  are untested.
