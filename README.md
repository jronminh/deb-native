# deb-native

Debian `arm64`, unmodified, in a directory on an Android phone: `apt`, `dpkg`,
`gcc`, `python3`, `git` running as they do on Debian, with no root, no `chroot`,
no namespaces and no machine emulation.

> **Pre-alpha** (0.9.0). AI-assisted and not independently audited
> ([`SECURITY.md`](SECURITY.md)). The threat model is accidental leaks, not
> isolation: this is not a sandbox.

The **T1 milestone** -- a Debian developer prefix -- is in: a real toolchain,
package management, and real `sudo`/`su`/`passwd` over one kernel uid.  What is
promised, best effort and refused is the
[**contract**](docs/spec/contract.md): containers, namespaces, kernel modules
and a desktop/GPU are deliberately out (a VM, not a prefix).

## How

The repository is the runtime's source and the recipe for a prefix; you build
both in Termux. Nothing prebuilt is downloaded except Debian's own packages.

- **`dn-tiny`**, a tracer with no libc, runs every program in the prefix under
  `ptrace` behind a seccomp filter. The syscalls that need it get their paths
  mapped into the prefix, root faked, or an answer Android will not give.
- **`dn-lane.so`**, a shim compiled in the prefix against its own glibc, does the
  same work inside each program for the calls that only read, including libc's
  own (its `svc` sites are patched at load), so they cost no stop.

The host kernel does the real work. What Android *hides* is mapped or answered;
what the kernel *lacks* is refused, not invented
([`contract.md`](docs/spec/contract.md)). A Lua build runs at about 13 s
(native Termux: 10 s); a `find` over 13,000 files takes about 0.13 s inside a
running session (native: 0.08 s; a fresh boot adds about 0.1 s). Details:
[`docs/architecture.md`](docs/architecture.md),
[`docs/performance.md`](docs/performance.md).

## Three ways to run a prefix

The runtime is one; a prefix is a tree; the **host** is who runs it:

| host | where the tree lives | who runs it | runtime |
| --- | --- | --- | --- |
| **Termux** | Termux's data (`~/debian`, ...) | the Termux app, app seccomp | app (default) |
| **custom app** | the app's data | a dedicated app (dn-shell), its session/UI | app (default) |
| **ADB** | `/data/local/tmp` | `adb shell` (uid 2000), no app, no app seccomp | `--host adb` |

The quick start below is the **Termux** host; the app contract is
[`spec/host.md`](docs/spec/host.md), the ADB host
[`spec/adb-host.md`](docs/spec/adb-host.md), and the overview
[`spec/hosts.md`](docs/spec/hosts.md).

## Quick start

In Termux, on an `arm64` phone:

```sh
pkg install git clang python debootstrap
git clone https://github.com/jronminh/deb-native
cd deb-native
scripts/build-prefix.sh ~/debian          # builds the runtime, then the prefix
~/debian/boot/boot.sh                     # a root shell in Debian
```

`scripts/build-prefix.sh --set dev ~/debian` adds a toolchain, `git`, `curl`
and `python3`; `--packages "..."` adds anything else. Everything is in
[`docs/building.md`](docs/building.md).

## Docs

| | |
| --- | --- |
| [`building.md`](docs/building.md) | build the runtime and a prefix; update one |
| [`architecture.md`](docs/architecture.md) | the tracer, the shim, the law, identity, the boundary |
| [`performance.md`](docs/performance.md) | the stop counts, the workloads, how to measure |
| [`spec/contract.md`](docs/spec/contract.md) | what a program may rely on: promised, best effort, refused |
| [`spec/identity.md`](docs/spec/identity.md) | one real uid, virtual principals, real sudo/su/passwd |
| [`spec/hosts.md`](docs/spec/hosts.md) | the three hosts: Termux, a custom app, ADB |
| [`spec/adb-host.md`](docs/spec/adb-host.md) | the ADB host: no app, a prefix under `/data/local/tmp`, driven from a PC |
| [`reference/`](docs/reference/) | the device census, the syscall classes, prior art, and the seccomp gates |
| [`tests/bench/`](tests/bench/) | the bench and check set (`run.sh PREFIX bench\|check\|all`) |

## Layout

| path | |
| --- | --- |
| `src/dn-tiny/` | the runtime: the tracer and the shim ([`README`](src/dn-tiny/README.md)) |
| `scripts/build-runtime.sh` | builds `dn-tiny` |
| `scripts/build-prefix.sh` | builds a prefix in Termux |
| `scripts/build-busybox.sh` | builds a busybox bundle, a tiny non-Debian tree ([`spec/variants.md`](docs/spec/variants.md)) |
| `scripts/adb-deploy.sh` | deploys an ADB-hosted prefix over `adb` ([`spec/adb-host.md`](docs/spec/adb-host.md)) |
| `scripts/adb-shell.sh` | `dn-adb`: enters an ADB-hosted prefix (refuses without adb) |
| `scripts/prefix/` | the prefix's `boot.sh` and `init.sh`, and the virtual-user wrappers |
| `tests/bench/` | the bench and check set behind `docs/performance.md` |
| `docs/` | building, architecture, performance, spec, reference |

The earlier runtime (`dn-monitor`, a PRoot fork) and its shipped prefixes are at
tag `legacy-dn-monitor`.

## License

GPL-3.0-or-later -- see [`LICENSE`](LICENSE) and [`CREDITS.md`](CREDITS.md).
