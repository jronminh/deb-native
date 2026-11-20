# Hosts: how a prefix runs

Status: **current** (2026-10-19), with one proposal.  The runtime is one; a **prefix** is a tree;
a **host** is who runs `dn-init` and how a person reaches it.  The tree, the
law and the prefix format are the same; the host decides the realm (app uid or
the ADB shell uid), where the tree lives, what the UI is, and which runtime
build it carries.

| host | who runs it | realm | where the tree lives | UI | Android seccomp |
| --- | --- | --- | --- | --- | --- |
| **Termux** | the Termux app | app uid (`untrusted_app_*`) | Termux's data (`~/debian`, ...) | a Termux terminal | the app filter |
| **custom app** | an app implementing the host contract | app uid | the app's data | the app's session/UI | the app filter |
| **ADB** | `adb shell` | shell uid 2000 | `/data/local/tmp` | a PC terminal | none |
| **dn-ng APK** | one APK (`targetSdk 36`) | app uid | the app's data | the app's session/UI | the app filter |

The app hosts (Termux, a custom app, and the dn-ng APK) have Android's seccomp
filter, so they use the **app runtime** (the default build), with the app-domain
answers (`policy_deny`, `accept`/`faccessat2` substitutes, ...).  The ADB host
has no app filter, so it uses the **adb runtime** (`--host adb`), which compiles
those answers out.  The rows, the filter, the law, identity and the shim are the
same in all current hosts.  dn-ng carries its own APK build ([`dn-ng.md`](dn-ng.md),
proposal).

**`targetSdk` is an app concept.**  The app hosts inherit their app's
`targetSdk` (scoped storage, W^X from app data, background rules, the per-SDK
app seccomp policy); the ADB host has none -- there is no manifest and the shell
runs with `Seccomp: 0`.  What remains there is the Android version, the OEM's
SELinux policy, and the kernel's config/version.

## Termux

The shortest path, and the one the quick start uses: build the runtime and the
prefix in Termux, then run `~/debian/boot/boot.sh` from Termux.  The runtime
runs as the **Termux app's uid**, under Android's app seccomp filter; the app
runtime is the right one.  The prefix is a directory in Termux's data, so
Termux's storage permission (when granted) governs `/sdcard`; a bind can expose
it.  No PC, no app of its own: a terminal is the whole host.

## Custom app

An Android app implementing the [host contract](host.md) owns the prefix's
storage, its sessions and the Android bridge. It starts `ROOT/boot/boot.sh`
under its app UID with the app runtime. The project exercised the custom
app model with the [retired dn-shell APK](../../archive/dn-shell/README.md).

## ADB

No app at all: the prefix lives in `/data/local/tmp`, `adb shell` runs as
**shell uid 2000** with **`Seccomp: 0`**, and a PC is the terminal.  The adb
runtime has no app-domain answers; the kernel's own gates (version, config,
SELinux/capabilities) remain.  The deployment, the `dn-adb` wrapper, the
`/sdcard` bind and the measurements are in [`adb-host.md`](adb-host.md).

The shell host can run a tree per device. A shell session depends on
`adbd` and does not start automatically at boot.
[FOA host domains](https://github.com/jronminh/fhs-on-android/blob/main/reference/domains.md)

## Choosing

- **Develop on the phone, no PC**: Termux.
- **A product with an app UI/session**: custom app.
- **One APK on a high `targetSdk`**: dn-ng (proposal).
- **A PC, a headless phone, CI, or another device as the terminal**: ADB.
- **Many phones as one cluster**: ADB, one tree per node, driven by a master.

The same **tree** can move between hosts with `tar`/`adb push` and the uid
reconcile in `init.sh`, but the **runtime build** must match the realm: app for
Termux and the custom app, adb for the shell.  `boot/version` records the host;
`dn-adb` refuses a prefix that is not `host=adb`, and an adb-built prefix must
not be booted from Termux/an app (it would miss the app's SIGSYS answers).

## Limits and security

No host is a sandbox: the prefix is the host app's (or the shell's) and any
process in it can reach what that identity can.  The app hosts inherit
Android's app sandbox; the ADB host inherits the much broader **shell** tier
(input, app control, system state) -- see [`adb-host.md`](adb-host.md).  Root,
namespaces and containers are out of scope ([`contract.md`](contract.md), T4);
display, input, audio and a GPU are the [device table](devices.md) and
[dn-ng](dn-ng.md) proposals.
