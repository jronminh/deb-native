# PTY and utmp

Status: **proposal** (2026-10-19).  T2 service emulation, not in 0.9.0.  There
are two separate gaps behind "login works":

- a **PTY pair** for `ssh`, `screen`, `tmux`, `script`, and anything that calls
  `posix_openpt`/`openpty`;
- the **utmp/wtmp/btmp/lastlog** files that login-style programs write and that
  `who`, `w`, `last`, and `su` read.

Neither is a kernel feature the app can synthesize: a PTY is a kernel object,
and the records are a shared file format.  The strategy differs.

## PTY: use the host if it lets us

The app domain may open `/dev/ptmx` and the host's devpts; on some Android
versions SELinux allows it, on others it does not.  The first step is a probe,
not a design.

- If `/dev/ptmx` opens and `TIOCGPTN`/`TIOCSPTLCK` work, the prefix can use the
  host's PTYs directly through the `/dev` host row.  `openpty`,
  `posix_openpt`, `grantpt`, `unlockpt`, `ptsname`, and termios then behave as
  on a Debian chroot.  The PTY is a real kernel object, shared with the host.
- If it does not open, there is **no userspace substitute** that preserves
  termios, job control, `SIGWINCH`, packet mode and `TIOC*` on the slave.  A
  socketpair is not a TTY; a shim that fakes `isatty()` breaks every program
  that then calls `tcgetattr`.  The honest answer is `ENODEV`/`EACCES`, with a
  boot-log note, not a fake PTY.
- The app already has one PTY per session (the terminal).  That PTY belongs to
  the host and is not exposed as `/dev/pts/N` to the tree; a program that wants
  a new PTY asks the kernel, and the probe decides.

The PTY is a **data** device: it can change what a program reads/writes, never
what it may do.

## utmp/wtmp: files, not a daemon

Debian programs use paths such as:

```
/run/utmp              current logins
/var/log/wtmp          login/logout history (last)
/var/log/btmp          failed logins (lastb)
/var/log/lastlog       per-uid last login
```

Provide them as **files in the tree**: create them at boot when missing, with
the fake ids and modes a Debian system expects (`utmp` group for `/run/utmp`,
`root:utmp 0664`, `wtmp`/`btmp` `root:utmp 0660`).  `/var/run` is a symlink to
`/run` in the tree, as on Debian.

- No daemon.  `login`, `sshd`, `su`, `screen`, `tmux` write the records
  themselves; `who`/`last`/`w` read them.  The runtime only provides the file
  and the right permissions; it does not synthesize records.
- Concurrency is the programs' problem, as on a normal Debian system.  Writes
  are small and most programs use `flock`/`fcntl` locks; a regular file keeps
  those semantics.
- The records never touch the host's `/var/run/utmp` or `wtmp`.  The prefix's
  files are its own; a host reader sees nothing.
- Rotation is not promised.  `logrotate` may exist in the tree and work; a
  missing logrotate leaves a growing file, bounded by the tree.
- `utmpname()` and alternate databases are libc/userland; whatever path the
  program names inside the tree is a normal file.
- The `struct utmp` layout is userland's, not the kernel's; it is consistent
  because every writer and reader in the prefix is the same arm64 glibc.

## What we do not do

- Do not fake `/dev/tty1..N`, `/dev/console` as a kernel VT, or `TIOCLINUX`.
  Android has no VT layer; a PTY is the honest console
  ([`devices/log.md`](devices/log.md) for `/dev/console` as a host console).
- Do not inject utmp records from the host session into the prefix; the app's
  session is not a Debian login.
- Do not make the host's PTYs appear under a per-prefix `/dev/pts` if the host
  devpts is shared; the `/dev` host row already exposes what the kernel has.

## Tests

- Probe `/dev/ptmx`: open, `grantpt`, `unlockpt`, `ptsname`, `tcgetattr`,
  `TIOCSWINSZ`, `SIGWINCH`, fork a shell on the slave.
- `script -q -c 'true' /dev/null`, `tmux`, `screen` (if installed).
- `who`, `w`, `last`, `lastb` after a login and a failed login.
- `su -` and (if present) `sshd` on a loopback port; confirm records land in the
  tree and not on the host.

## Milestones

| | scope |
| --- | --- |
| **Y1** | probe the host devpts; document pass/refuse per device |
| **Y2** | seed `/run/utmp`, `/var/log/{wtmp,btmp,lastlog}`; `who`/`last` read |
| **Y3** | `login`/`su`/`sshd` write records; `screen`/`tmux` PTY paths |

## Non-goals

- A userspace PTY that pretends to be a kernel TTY.
- A VT (`/dev/tty1..N`), `fbcon`, or a kernel console.
- Sharing utmp with Android.
- A login manager/daemon.

## Open questions

1. **devpts on the target phones**: allowed on the S10e and on Android 16?
   The answer decides whether Y3 is possible at all.
2. **Path set**: some programs use `/var/run/utmp` (symlink) and some use
   `/run/utmp`; both must resolve in the tree.
3. **File seeding**: create empty files at boot, or let the first writer create
   them?  Empty files make `who` quiet; missing files make some programs log an
   error.
4. **`lastlog`**: sparse file by uid; the fake uid is the app's, so the record
   is for one virtual user.  Is that honest enough?
5. **PTY leak**: if the host devpts is visible, a guest can see host PTYs; is a
   narrower `/dev/pts` row possible without breaking the kernel's devpts?

## Sources

The Linux pty/devpts documentation and `termios(3)`; glibc's `utmp`/`utmpx`
interfaces for the record layout; `script(1)`, `screen`, `tmux`, `who`, `last`,
and `sshd` as the clients.  There is no userspace PTY substitute; the probe of
`/dev/ptmx` is the decision.
