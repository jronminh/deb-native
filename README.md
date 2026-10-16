# deb-native

Debian `arm64`, unmodified, in a directory on an Android phone: `apt`, `dpkg`,
`gcc`, `python3`, `git` running as they do on Debian, with no root, no `chroot`,
no namespaces and no emulation.

> **Pre-alpha** (0.9.0). AI-assisted and not independently audited
> ([`SECURITY.md`](SECURITY.md)). The threat model is accidental leaks, not
> isolation: this is not a sandbox.

## How

The repository is the runtime's source and the recipe for a prefix; you build
both in Termux. Nothing prebuilt is downloaded except Debian's own packages.

- **`dn-tiny`**, a tracer of about 2,000 lines with no libc, runs every program
  in the prefix under `ptrace` behind a seccomp filter. The few syscalls that
  need it get their paths mapped into the prefix, root faked, or an answer
  Android will not give.
- **`dn-lane.so`**, a shim compiled in the prefix against its own glibc, does the
  same work inside each program for the calls that only read, including libc's
  own (its `svc` sites are patched at load), so they cost no stop.

A gcc build runs at about the speed of a native compiler; a `find` over 13,000
files takes 60 ms. Details: [`docs/architecture.md`](docs/architecture.md),
[`docs/performance.md`](docs/performance.md).

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

## Layout

| path | |
| --- | --- |
| `src/dn-tiny/` | the runtime: the tracer and the shim ([`README`](src/dn-tiny/README.md)) |
| `scripts/build-runtime.sh` | builds `dn-tiny` |
| `scripts/build-prefix.sh` | builds a prefix in Termux |
| `scripts/prefix/` | the prefix's `boot.sh` and `init.sh` |
| `docs/` | [building](docs/building.md), [architecture](docs/architecture.md), [performance](docs/performance.md), [reference](docs/reference/) |

The earlier runtime (`dn-monitor`, a PRoot fork) and its shipped prefixes are at
tag `legacy-dn-monitor`.

## License

GPL-3.0-or-later — see [`LICENSE`](LICENSE) and [`CREDITS.md`](CREDITS.md).
