# Phase 2: show the real kernel, hide the runtime

Phase 1 ([`plan.md`](plan.md), [`coverage.md`](coverage.md)) maps every coordinate
the kernel can carry.  Phase 2 answers the values the kernel *fixes* and a mapping
cannot carry.  Its rule is the model's: **map over fake, fake is the residue,
refuse when a fake would be a lie** -- and here the residue is small.

## Real, not faked

The phone is a real **Linux** kernel on real **arm64** hardware.  That is the
truth, and it is shown as it is -- there is nothing to invent:

- `uname` (sysname, release, version, machine), `/proc/version`, `/proc/cpuinfo`;
- `/proc/uptime`, `/proc/loadavg`, `/proc/stat`, `/proc/vmstat` (the kernel's own
  numbers, **if** the app domain lets the tracer read them);
- `/sys`, `/dev`.

A program that reads the kernel sees the kernel.  "Refuse, don't fake" sets the
floor: no invented kernel version, no invented CPU.

## Forced fakes (and the Debian identity they carry)

Only what Android *forces* is changed -- the runtime's own traces, and the files
the app domain denies.  Each such answer is the prefix's, and says so: the string
it emits names the Debian release the prefix reproduces (`boot/version`:
`Debian <suite>`, `version=`).  So a fake is *honest* -- it is the runtime
telling you which Debian it is, not pretending to be another kernel.

- **`/proc/<pid>/status`**: `TracerPid` is `dn-tiny` and `Seccomp` is 2 -- traces
  of *our* runtime, not the kernel.  Both are answered as a plain process's
  (0 and none); `Name`/`State` are the program's, and `Uid`/`Gid` are the fake
  root's, so a `ps` that reads the file sees the same root the syscalls report.
- **`/proc/<pid>/cmdline`**: the kernel's line names the loader the exec gate
  adds (`ld.so ... --preload <host path> ...`); it is answered with the argv the
  program was run with (the gate's prefix dropped), so no host path shows.
- **`/proc/<pid>/comm`**: the kernel's names the loader; it is answered with the
  program.
- **ids**: fake root (done).
- **the SELinux label**: absent (done, boundary 2).
- **`/etc/os-release`, `dpkg --print-architecture`**: the tree's own Debian files,
  kept as they are (no fake needed).
- **the denied `/proc` files**: the real value if the tracer can read it, else a
  clear refusal -- never invented numbers.  (A synthesized line, if one is ever
  needed, carries the Debian identity.)
- **the mount view**: its source is the runtime, `deb-native` -- the one string
  that says "this is a deb-native tree" (a Debian `<suite>`).

## The order of answering: map, ask the kernel, refuse

Every `/proc` file is one *presentation* of a kernel truth; Android may block the
presentation (the file) while the truth is still there.  So, in order:

1. **Map** -- a coordinate (a path): the law, guest to host (Phase 1).
2. **Ask the kernel by another call** -- the file is denied but the truth has a
   syscall: `/proc/uptime` is `clock_gettime(CLOCK_BOOTTIME)`, `/proc/loadavg` is
   `sysinfo`.  Nothing is invented.
3. **Refuse** -- no syscall gives the truth (`/proc/stat`, `/proc/vmstat`: no
   aggregate CPU or VM counters outside the file), so it stays refused and the
   tool degrades.  Or **fake, and say so** (the Debian identity) when a fake is
   forced, as `/proc/<pid>/status` must drop the runtime's own traces.

## An accepted residue: PPid

`PPid` and `getppid()` name dn-tiny: the tracer really is the parent, and the
parent-child link is load-bearing (ptrace, `wait`, signal delivery).  Faking the
reported value would not change the real one -- only break the consistency a
program can check (`getppid()` against `/proc/1`, orphan detection) and hide one
clue while the parent process (its `/proc/<tracerpid>/comm`, `ps`) still shows.
So it is left the truth, like the kernel and the hardware: a program that looks
finds the tracer, a program that does not ask does not.

## The fingerprint test

`scripts/fingerprint.sh PREFIX`, the mirror of the coverage test: it reads the
kernel and hardware and **requires them to be the host's** (the same kernel), and
requires the runtime's traces to be gone -- `TracerPid` 0, no `Seccomp`, no
SELinux label, no `android` string -- while the Debian identity is the tree's
(`ID=debian`, `dpkg --print-architecture` = arm64).  A deliberate prober may
still find the phone; this is about what a program finds *without asking*.

## Order

1. the fingerprint test (A) -- done;
2. `/proc/<pid>/status` (D) -- done: TracerPid 0, no Seccomp, State running, Name
   the program, Uid/Gid root's; and the process view beside it (`cmdline` with
   the gate's loader dropped, `comm` the program);
3. the denied files (C) -- `/proc/uptime` and `/proc/loadavg` are answered with
   the real values (`CLOCK_BOOTTIME` and `sysinfo`), generated at open, so
   `uptime` works; `/proc/stat` and `/proc/vmstat` get a **fixed placeholder**
   instead of `EACCES`: every counter 0 with `btime 0` (stat) and a
   `dn_not_faked 1` key (vmstat), so the file is readable but says at once it is
   not the kernel's -- a fake is avoided, only a null is shown (`top` and
   `vmstat` run again, and a reader knows);
4. capabilities and cgroup (E), then rlimit/`/proc/sys` (F).  Capabilities come
   with the fake root: `/proc/<pid>/status` shows root's set (not 0), since a root
   with no capabilities is the inconsistency.  `/proc/<pid>/cgroup` is answered
   `0::/` -- Android's line leaks the app's uid and pid.  The rlimits and the
   readable `/proc/sys` are the kernel's own (left as they are); the denied ones
   (`/proc/sys/kernel/pid_max` 4194304, `/proc/sys/vm/max_map_count` 65530,
   `/proc/sys/kernel/cap_last_cap` 40 -- the Debian kernel's defaults, a constant
   each, not the phone's live value) are answered from a fixed value.

## Open

- the identity string a fake emits: the mount source `deb-native`, or
  `deb-native:Debian <suite>` -- to settle when a fake first needs it.
