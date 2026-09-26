# Runtime failure modes

What goes wrong when **running** a prefix program (as opposed to installing
it), grouped by cause. Each is tagged with what handles it: **shim** = our
libc interposer (`native/path-redirect.c`), **tracer** = fork-lite
(`tracer/`), **—** = nothing today. Coverage of the shim itself is in
[`shim-coverage.md`](shim-coverage.md); the syscall boundary in
[`syscall-boundary.md`](syscall-boundary.md).

## A. Path / filesystem access

- **Static binaries** (Go, musl, `bash-static`) — no dynamic linker, no shim →
  they read the real root. *tracer*
- **Inline `svc` in dynamic binaries** (Go/Rust cgo, inline asm) — syscalls
  bypass libc. *tracer*
- **Explicit `syscall()`** — a public symbol, so interposable in principle;
  not today. *shim (TODO)*
- **libc-internal opens** (NSS, `__open_nocancel`, loader) — never cross the
  PLT. *tracer*
- **Symlink targets** — the kernel follows an absolute `/usr/…` target against
  the *real* root; the shim only rewrites the argument.
  `normalize-symlinks.sh` fixes prefix symlinks, but runtime-created or
  outside-prefix ones escape. *partial*
- **`/proc`, `/sys`** — not redirected; programs see Android reality
  (`/proc/mounts`, `/proc/self/…`).
- **`/tmp`, `/run`, `/var/run`** — `/tmp` is not one of our four directories
  and Android has no real `/tmp`; `/run` is not redirected (only `/var/run`).
  Common write/ENOENT failures. *—*
- **Hard links / cross-FS `rename`** — prefix root vs `/tmp`/sdcard; some
  Android filesystems cannot hardlink. *—*
- **Uncovered syscalls** — `io_uring`, `open_by_handle_at`, the new mount API,
  `fanotify_mark`, `sendmmsg`. *tracer (mostly)*

## B. Dynamic loading

- **Absolute `DT_NEEDED`/`RUNPATH`** (`/usr/lib/…`) — the loader opens
  internally, not through the PLT. Mitigated by `grun --configure` and Termux
  glibc's own search path, not universally. *partial*
- **`/etc/ld.so.cache`, `/etc/ld.so.preload`** — read from the real root.
- **NSS / `gconv` / locale modules** — loader-internal absolute paths; a
  missing `gconv-modules` can abort `iconv` (and thus many programs).

## C. Exec & process creation

- **Shebang under a Bionic parent** — the kernel resolves `#!/bin/sh` against
  the real root; the shim rewrites it only when the *caller* is shimmed.
- **Environment cleared** (`env -i`, setuid, `sudo`) → `LD_PRELOAD` lost →
  **no shim at all**. *bake into ELFs / `DT_AUDIT` (TODO)*
- **Launcher-only** — running a prefix binary by absolute path, not through a
  generated launcher, sets no shim.
- **`system()` / `popen` / `posix_spawn` / `execveat` / `fexecve`** — mostly
  covered (`posix_spawn` yes); fd-based exec and libc-internal spawns can slip.
- **Bionic children** — the shim deliberately strips the preload, so
  redirection ends at that fork (by design).

## D. Identity & OS assumptions

- **`uname` reports `Android`**, and **`os-release` exists only under the
  shim** → static binaries and anything evading the shim see "other-linux".
  This is exactly Tailscale's installer failure
  ([`tailscale.md`](tailscale.md)).
- **No systemd / dbus / `lsb_release`** → programs that branch on them take
  wrong paths or fail (`systemctl`, `sd_notify`).
- **Distro / codename branches** in installers and apps.

## E. Privilege & kernel

- **Root-only ops** — mount, `pivot_root`, `chroot`, `swapon`, netlink,
  netfilter, TUN, raw sockets, ports <1024, `mknod`.
- **setuid/setgid/file caps** — do not work (and `ld.so` strips the preload
  for them anyway).
- **SysV IPC** (`shmget`/`semget`/`msgget`) — blocked; fork-lite also dropped
  proot's sysvipc extension, so these regress.
- **`/dev/shm`, `memfd`** — may be absent.
- **seccomp / SELinux** — an app-wide filter stacks; some syscalls are denied
  regardless.
- **System users/groups** (`adduser`).

## F. Services & process model

- **No init/service manager** — `systemctl`/`service`/`update-rc.d` no-op or
  fail; daemons must be started by hand.
- **No reaper** — double-forked children accumulate as zombies.
- **Logging** — `/var/log`, journald absent; syslog sockets may not exist.
- **`/run` sockets, PID files** — path mismatches (see A).
- **DNS/resolvconf** assumptions (`systemd-resolved`); userspace networking
  avoids some of this by design.

## G. Delivery / environment

- **`LD_PRELOAD` fragility** (see C) — the shim is a convention, not a
  guarantee; any environment reset silently disables redirection.
- **PATH ordering** — redirection only applies when launched through our
  launchers.
- **Two package managers sharing `$HOME`** — `~/.config`, `~/.cache`,
  `~/.local` collide; `update-alternatives` writes into Termux's own prefix
  (known nit).

## H. Tracer-specific (once fork-lite is in)

- **`io_uring`** not intercepted (the path is in a shared ring).
- **`ptrace` overhead**; proot's `-b` requires the host bind path to exist.
- **`clone3` / `vfork` / `fexecve`** tracee-tracking edge cases.
- **Dropped extensions** (`link2symlink` for Android hardlink quirks,
  `sysvipc`) may resurface.

## Highest risk in practice

1. **Env reset / static / inline syscalls** → silent loss of redirection
   (needs the tracer and/or baking the shim into ELFs).
2. **`/tmp` and `/run` not redirected** → very common write / ENOENT failures.
3. **Loader-internal absolute paths** (NSS/gconv/locale, absolute
   `DT_NEEDED`).
4. **No systemd/dbus** → anything service-shaped.
5. **Root-only network/namespace ops** → TUN, iptables, raw sockets
   (Tailscale's exact wall).
