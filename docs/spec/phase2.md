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
  (0 and none).
- **ids**: fake root (done).
- **the SELinux label**: absent (done, boundary 2).
- **`/etc/os-release`, `dpkg --print-architecture`**: the tree's own Debian files,
  kept as they are (no fake needed).
- **the denied `/proc` files**: the real value if the tracer can read it, else a
  clear refusal -- never invented numbers.  (A synthesized line, if one is ever
  needed, carries the Debian identity.)
- **the mount view**: its source is the runtime, `deb-native` -- the one string
  that says "this is a deb-native tree" (a Debian `<suite>`).

## The fingerprint test

`scripts/fingerprint.sh PREFIX`, the mirror of the coverage test: it reads the
kernel and hardware and **requires them to be the host's** (the same kernel), and
requires the runtime's traces to be gone -- `TracerPid` 0, no `Seccomp`, no
SELinux label, no `android` string -- while the Debian identity is the tree's
(`ID=debian`, `dpkg --print-architecture` = arm64).  A deliberate prober may
still find the phone; this is about what a program finds *without asking*.

## Order

1. the fingerprint test (A);
2. `/proc/<pid>/status` (D) -- the biggest trace of the runtime;
3. the denied files (C), reading the kernel where it can;
4. capabilities (E), then cgroup/rlimit/`/proc/sys` (F).

## Open

- the identity string a fake emits: the mount source `deb-native`, or
  `deb-native:Debian <suite>` -- to settle when a fake first needs it.
