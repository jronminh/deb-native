# deb-native

**A common runtime for Linux software on Android.**  The goal is one platform
under an Android app — a terminal today, and a host for GUI programs, tools,
games and emulators tomorrow — with the programs unmodified and the phone's
kernel doing the real work: no root, no `chroot`, no namespaces, and no CPU
emulation in the core.

Debian `arm64` is the **first tree**; the runtime is the platform.  A tree is
data; a host is the app or shell around it.

> **Pre-alpha** (0.10.0).  AI-assisted, independently unaudited
> ([`SECURITY.md`](SECURITY.md)).  Scope: compatibility; use a VM for
> isolation.

## What exists today

The **T1 milestone** — a Debian developer prefix — is in: a real toolchain,
package management, and real `sudo`/`su`/`passwd` over one kernel uid.  On a
stock arm64 phone:

- `apt`, `dpkg`, `gcc`, `python3`, `git`, `bash` and the rest of a Debian
  userland run unmodified;
- a terminal session works in Termux, in a custom app, or over ADB;
- the [contract](docs/spec/contract.md) says what is promised, best effort,
  and refused.

0.10.0 covers the terminal and tools.  GUI, GPU, audio and Steam come next, with
designs: the [device table](docs/spec/devices.md) for display/input,
[dn-ng](docs/spec/dn-ng.md) for a high-`targetSdk` APK, and the device models
next to them.  Steam and CPU emulators build on extra layers (Wine, Box64/FEX,
a GPU backend); the core stays same-architecture.

## The model

What Android *hides* is mapped or answered; what the kernel *lacks* is refused
([`contract.md`](docs/spec/contract.md)).  That is the contract that lets an
unmodified tree run.

- **`dn-init`**, a tracer with no libc, runs every program in the prefix under
  `ptrace` behind a seccomp filter.  The syscalls that need it get their paths
  mapped into the prefix, root faked, or an answer the host hides.
- **`dn-lane.so`**, a shim preloaded into each dynamic program, does the same
  work in-process for the calls that only read, including libc's own (its `svc`
  sites are patched at load), so they cost no stop.
- **`dn-exec`**, a userland exec, maps the loader itself and patches the
  loader's -- or a static program's -- syscall sites at birth.  It replaces the
  kernel's `execve` for every program the tree starts.
- **One law** (`law.c`) is shared by all three; two path models would disagree.
- A **host** supplies what the framework owns: the session, boot inputs,
  Android APIs, and later display/input/audio through the device table.

A Lua build runs at about 13 s (native Termux: 10 s); a `find` over 13,000
files takes about 0.13 s inside a running session (native: 0.08 s; a fresh boot
adds about 0.1 s).  Details: [`docs/architecture.md`](docs/architecture.md),
[`docs/performance.md`](docs/performance.md).

## Where it is going

| track | today | direction |
| --- | --- | --- |
| **terminal / tools** | Debian T1, three current hosts | stable, near-native |
| **GUI** | next | a host backend (X11/Wayland) + the device table |
| **input / audio** | next | evdev/uinput, ALSA/AAudio; data only |
| **games / Steam** | roadmap | same-arch tools first; Wine/Box64/GPU are extra layers |
| **emulators** | roadmap | same-arch emulators as apps; CPU emulation lives in the app |
| **APK on high Android** | legacy app at `targetSdk 28` | [dn-ng](docs/spec/dn-ng.md): APK-only, `targetSdk 36` |
| **isolation** | VM path | AVF, seL4/CHERI |

The security research behind this boundary — and the bypasses a host has to
respect — is in
[`docs/reference/syscall-security.md`](docs/reference/syscall-security.md).

## Three hosts, and a fourth coming

The runtime is one; a prefix is a tree; the **host** is who runs it:

| host | where the tree lives | who runs it | runtime |
| --- | --- | --- | --- |
| **Termux** | Termux's data (`~/debian`, ...) | the Termux app, app seccomp | app (default) |
| **custom app** | the app's data | a dedicated app (dn-shell), its session/UI | app (default) |
| **ADB** | `/data/local/tmp` | `adb shell` (uid 2000), no app, no app seccomp | `--host adb` |
| **dn-ng APK** | the app's data | one APK, high `targetSdk`, userland exec | APK libs |

The quick start below is the **Termux** host; the app contract is
[`spec/host.md`](docs/spec/host.md), the ADB host
[`spec/adb-host.md`](docs/spec/adb-host.md), the overview
[`spec/hosts.md`](docs/spec/hosts.md), and the next-generation APK
[`spec/dn-ng.md`](docs/spec/dn-ng.md).

## Quick start

In Termux, on an `arm64` phone:

```sh
pkg install git clang python debootstrap
git clone https://github.com/jronminh/deb-native
cd deb-native
scripts/runtime/build-runtime.sh                 # dn-init, dn-exec
scripts/runtime/build-shim.sh --cross            # dn-lane.so (or --in-prefix P)
scripts/prefix/build-prefix.sh ~/debian          # the tree, from those three
~/debian/boot/boot.sh                     # a root shell in Debian
```

`scripts/prefix/build-prefix.sh --set dev ~/debian` adds a toolchain, `git`, `curl`
and `python3`; `--packages "..."` adds anything else. Everything is in
[`docs/building.md`](docs/building.md).

## Docs

| | |
| --- | --- |
| [`building.md`](docs/building.md) | build the runtime and a prefix; update one |
| [`architecture.md`](docs/architecture.md) | the tracer, the shim, the law, identity, the boundary |
| [`performance.md`](docs/performance.md) | the stop counts, the workloads, how to measure |
| [`spec/contract.md`](docs/spec/contract.md) | what a program may rely on: promised, best effort, refused |
| [`spec/devices.md`](docs/spec/devices.md) | synthetic `/dev` devices: the device table, provider protocol (proposal) |
| [`spec/dn-ng.md`](docs/spec/dn-ng.md) | next-generation APK runtime for a high `targetSdk` (proposal) |
| [`reference/syscall-security.md`](docs/reference/syscall-security.md) | restricted syscalls through a security lens: bypass classes, research, rules |
| [`spec/identity.md`](docs/spec/identity.md) | one real uid, virtual principals, real sudo/su/passwd |
| [`spec/hosts.md`](docs/spec/hosts.md) | the hosts: Termux, a custom app, ADB |
| [`spec/host.md`](docs/spec/host.md) | what a custom app provides a prefix |
| [`spec/adb-host.md`](docs/spec/adb-host.md) | the ADB host: no app, a prefix under `/data/local/tmp`, driven from a PC |
| [`reference/`](docs/reference/) | the device census, the syscall classes, prior art, and the seccomp gates |
| [`tests/bench/`](tests/bench/) | the bench and check set (`run.sh PREFIX bench\|check\|all`) |

Emulation proposals (T2/T3): [`spec/ipc-sysv.md`](docs/spec/ipc-sysv.md),
[`spec/ipc-posix.md`](docs/spec/ipc-posix.md), [`spec/pty-utmp.md`](docs/spec/pty-utmp.md),
[`spec/netlink.md`](docs/spec/netlink.md), and the device models under
[`spec/devices/`](docs/spec/devices/).

## Layout

| path | |
| --- | --- |
| `src/dn-init/` | the tracer, the exec gate, and the shim's source ([`README`](src/dn-init/README.md)) |
| `src/dn-exec/` | the userland exec ([`architecture.md`](docs/architecture.md)) |
| `scripts/runtime/` | builds the runtime: `build-runtime.sh`, `build-shim.sh` |
| `scripts/prefix/` | builds a tree: `build-prefix.sh`, `build-busybox.sh`, and the `boot.sh`/`init.sh` it copies |
| `scripts/host/` | the ADB host: `adb-deploy.sh`, `adb-shell.sh` (`dn-adb`) ([`spec/adb-host.md`](docs/spec/adb-host.md)) |
| `scripts/measure/` | `bench.sh`, `coverage.sh`, `fingerprint.sh` and their probes |
| `tests/bench/` | the bench and check set behind `docs/performance.md` |
| `docs/` | building, architecture, performance, spec, reference |

The earlier runtime (`dn-monitor`, a PRoot fork) and its shipped prefixes are at
tag `legacy-dn-monitor`.

## License

GPL-3.0-or-later -- see [`LICENSE`](LICENSE) and [`CREDITS.md`](CREDITS.md).
