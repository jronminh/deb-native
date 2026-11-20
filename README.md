# deb-native

deb-native builds and runs a rootless Debian `arm64` FHS tree on Android.
It runs Debian programs on the phone's kernel from a Termux terminal or
over ADB. It also defines the runtime contract for a custom Android host.

Built on [FOA](https://github.com/jronminh/fhs-on-android/blob/885730c9848d2a2aa5b8063f6e234f5641c5611e/README.md) -> [methods/tracer.md](https://github.com/jronminh/fhs-on-android/blob/885730c9848d2a2aa5b8063f6e234f5641c5611e/methods/tracer.md) (level 3), with [loader](https://github.com/jronminh/fhs-on-android/blob/885730c9848d2a2aa5b8063f6e234f5641c5611e/methods/loader.md)/[exec](https://github.com/jronminh/fhs-on-android/blob/885730c9848d2a2aa5b8063f6e234f5641c5611e/methods/exec.md)/[identity](https://github.com/jronminh/fhs-on-android/blob/885730c9848d2a2aa5b8063f6e234f5641c5611e/methods/identity.md) (level 2).

FOA's levels describe how much sits between the program and Android.
`dn-lane.so` and `dn-exec` handle eligible work in process. `dn-init`
traces the tree and backs those fast paths, including virtual identity.
The runtime runs under the host's Android permissions and provides
compatibility only. [FOA method index](https://github.com/jronminh/fhs-on-android/blob/885730c9848d2a2aa5b8063f6e234f5641c5611e/methods/README.md)

## Status and support

**0.10.0 pre-alpha.** The T1 milestone covers the terminal and development
tools. A Debian developer prefix runs `apt`, `dpkg`, `gcc`, `python3`, `git`
and `bash`. Guest `sudo`, `su` and `passwd` use virtual principals over a
single Android UID. See the [runtime contract](docs/spec/contract.md) and
[security policy](SECURITY.md).

| Host | How it starts | Runtime |
| --- | --- | --- |
| [Termux](docs/spec/hosts.md#termux) | A prefix under Termux's app data | Default app build |
| [ADB](docs/spec/adb-host.md) | A prefix under `/data/local/tmp`, started as Android's shell UID | `--host adb`, including the tracer |
| [Custom app](docs/spec/host.md) | The app owns storage, sessions and Android integration | Default app build |

The [retired dn-shell APK](archive/dn-shell/README.md) exercised the custom
app model. The [dn-ng APK](docs/spec/dn-ng.md) and [device providers](docs/spec/devices.md)
are proposals. The [translated apt feed](apt-feed/README.md) is a historical
level-1 proposal for the 0.2 runtime.

## Build and run

The [build guide](docs/building.md) covers building, booting and updating a
prefix. Build the runtime with AArch64 Clang in Termux or on a Linux build
host. A first prefix needs a glibc-linked shim built from a Linux toolchain with
`aarch64-linux-gnu-gcc` and a sysroot compatible with the target tree.
An existing development prefix can build the shim with `--in-prefix`.

After placing the app runtime and shim in the checkout's `build/` directory,
build and enter a prefix on the phone in Termux.

```sh
scripts/prefix/build-prefix.sh --set dev ~/debian
~/debian/boot/boot.sh
```

For ADB, the second stage still runs in Termux with the app runtime. The
builder then installs the ADB runtime. Follow the
[complete ADB build sequence](docs/building.md#an-adb-hosted-prefix), then
[deploy the prefix](docs/spec/adb-host.md#deploy).

## Repository guide

| Where | What it covers |
| --- | --- |
| [Source](src/README.md) and [architecture](docs/architecture.md) | Tracer, shim, userland exec and the path and identity rules they share |
| [Build scripts](scripts/) | Runtime builds, prefix bootstrap, ADB deployment and measurement |
| [Performance](docs/performance.md) and [regression suite](tests/bench/README.md) | Measured workloads, counters and device checks |
| [Runtime contract](docs/spec/contract.md) and [host overview](docs/spec/hosts.md) | Guarantees, limitations and host profiles |
| [FOA references](refs/foa.md) | How this implementation relates to FOA's methods, recipes and evidence |
| [Research notes](notes/README.md) and [roadmap](TODO.md) | Selected measurements and planned work |

GPL-3.0-or-later. See [LICENSE](LICENSE) and [CREDITS](CREDITS.md).
