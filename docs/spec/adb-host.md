# The ADB host: a prefix without an app

Status: **implemented and tested** (2026-10-18) on the S10e, Android 12.
The runtime is the same; the host is different: instead of an app, the prefix
lives in `/data/local/tmp` and is driven from a PC over `adb shell`.  This is
**one of the hosts** -- Termux, a custom app and the dn-ng APK are the others
([`hosts.md`](hosts.md)).  It is a **host variant**, not a kernel or a fork: one
source, one law, the same rows.

## The model

- **No app.** The prefix is a directory under `/data/local/tmp` (shell-owned,
  persistent until an OTA or a wipe).  `adb push` and `adb shell` are the
  transport; the PC is the client.
- **The shell uid.** `adb shell` runs as `uid=2000(shell)`,
  `context=u:r:shell:s0`, with **`Seccomp: 0`** -- there is no app seccomp filter.
  It has more than an app (execute from `/data/local/tmp`, `/sdcard`, `logcat`,
  some system services) and less than root (no `chown`/`mount`/`setuid`, no
  other apps' data).
- **No `targetSdk`.**  That is an app concept: the app-only behaviours (scoped
  storage, W^X from app data, background rules, the per-SDK seccomp policy) do
  not apply to the shell.  The Android version, the OEM's SELinux policy and the
  kernel's config/version do.
- **The app's seccomp answers are not built.**  A runtime built with
  `--host adb` compiles out `policy_deny`, `answer_sigsys`, the substitutes and
  the `deny_count` stats.  The rows, the filter, the law, identity and the
  shim are unchanged.  `sigsys=0` in the summary.

## Build

```sh
scripts/build-runtime.sh --host adb build-adb          # the shell/ADB runtime
scripts/build-prefix.sh --host adb \
    --runtime build --runtime-adb build-adb --set minimal ~/p-adb
```

The prefix is **built** with the app runtime (`--runtime`): the second stage and
the shim run in Termux, where Android's app filter is active.  Only at the end
is the adb runtime (and its loader hook, if any) put in place; the shim is
compiled with `-DDN_HOST_ADB`.  `boot/version` records `host=adb`.

The profile also writes `boot/binds`:

```
/sdcard	/storage/emulated/0	ro
```

and `init.sh` **reconciles** the real uid: a prefix built under another host
carries that host's `user` line, so the old entry is removed and `user` is
written for the real uid at boot (builder 10323 -> shell 2000).

## Deploy

```sh
scripts/adb-deploy.sh --port 39063 ~/p-adb.tar.gz
```

It connects, pushes the tarball, extracts under `/data/local/tmp/<name>`,
checks `boot/version` says `host=adb`, and boots once as `shell`.  The extraction
by an unprivileged user drops the setuid bits; the prefix carries `boot/setuid`
(written by `build-prefix.sh`) and `init.sh` restores them on the first boot, so
`sudo`/`su`/`passwd` work.  (A prefix built before the manifest needs the list
added once.)  Later:

```sh
adb shell 'cd /data/local/tmp/p-adb && ./boot/boot.sh'
```

The first boot may take a moment (init.sh writes passwd/group for uid 2000).

## Enter the shell

`scripts/adb-shell.sh` is the wrapper; install it once as `dn-adb`:

```sh
ln -sf "$PWD/scripts/adb-shell.sh" ~/.local/bin/dn-adb
DN_ADB_PORT=39063 dn-adb                  # an interactive shell in the prefix
DN_ADB_PORT=39063 dn-adb -c 'id; pwd'     # one command
dn-adb --prefix /data/local/tmp/p-adb     # another prefix
```

It **refuses without an ADB device** (there is no local fallback) and refuses a
prefix whose `boot/version` is not `host=adb`.  `--port`/`DN_ADB_PORT` connects
first; otherwise an already-connected device is used.  The default prefix is
`/data/local/tmp/p-adb` (`--prefix`/`DN_ADB_PREFIX` changes it).  The `-c` path
feeds the command to the prefix's `sh` on stdin, so quoting stays local; the
interactive path uses `adb shell -t`, so a real terminal gets a PTY.

## What it gives (measured 2026-10-18, S10e, kernel 4.14)

| test | result |
| --- | --- |
| `id` | `uid=2000(user)`, the shell's groups |
| `DN_INITIAL=root id` | `uid=0(root)` (fake root) |
| `chown 0:0` | root seed: 0 and `0:0`; real user: `EPERM` |
| `unshare(CLONE_NEWUSER)` / `(CLONE_NEWNS)` | `EINVAL` / `EPERM` |
| `shmget`, `msgget`, `semget` | `ENOSYS` (`CONFIG_SYSVIPC` off) |
| `clone3` | `ENOSYS` (kernel 4.14) |
| `setuid(0)`, `setgroups` | `EPERM` (row fake for the root seed) |
| `apt-get update` | 0 |
| `apt-get install hello` + run | `Hello, world!` (root seed) |
| `/sdcard` | the bind reads `DCIM`, `Android`, ... |
| `bind("/tmp/x.sock")` | an abstract name (`shell` may not create a `sock_file`); `getsockname` shows the path, a placeholder file gives `stat`/`chmod`/`unlink` |
| `dn-tiny` summary (`id`) | `enter=136 exit=0 sigsys=0`, 0.2 s |

So the **seccomp gate is gone**, while the kernel's own gates stay: version,
config, SELinux/capabilities.  The adb host gains `/sdcard` and loses nothing
from the app except the app lifecycle and data directory.

## Limits

- **Not a sandbox and not root.** The shell uid is a serious privilege tier
  (it can inject input, dump system state, control apps); only run trusted code.
  ADB authorization is the boundary.  The tree is the shell's, not an app's.
- No boot autostart (no root/init); the PC must connect and start it, or a
  `setsid`/`nohup` child can outlive a session (bound to the shell's limits).
- `/data/local/tmp` survives reboots but can be wiped by an OTA; no encryption.
- **libc text is not patched.**  The `shell` domain allows a private COW text
  page (the `xm` probe's `RW -> write -> RX` passes), but the shim's old
  `RWX -> RX` route left the page in a state the probe rejected; the ADB build
  (`DN_HOST_ADB`) now skips the libc `svc` patch, and the profile does not ship
  `dn-ldhook.so` either.  The exported wrappers and the tracer carry the calls,
  and `tests/bench/xm` passes.  A COW patch is the next step.
- **A pathname socket is abstract, with a placeholder file.**  The guest path
  becomes an abstract name (`\0dn-<hash>`, from the root and the resolved host
  path), because `shell` has no `sock_file create` on `shell_data_file`; bind
  also makes a regular placeholder at the path, so `chmod` and `unlink` work; a
  shared table (`var/lib/deb-native/sockets`) makes the shim's `stat` report
  `S_IFSOCK`.  `lstat`/`readdir` still see the placeholder, and a raw/static
  `stat` (no shim) does too.  `ssh-agent`/`ssh-add` and `tmux` are checked.
- `CONFIG_USER_NS` is off kernel-wide; namespaces, overlayfs, FUSE and mount
  stay refused.  Containers are a VM, not this host.
- The **self-hosted** variant (`adbwire`/`dsh` from
  [`termux-adb-bridge`](https://github.com/jronminh/termux-adb-bridge)) is the
  no-PC option: one Wireless-Debugging pairing, then a shell-uid daemon.  It was
  vendored in the old `prefix/` fork (`d9fa544`) and dropped with it
  (`66982d9`); it is not needed for the PC-hosted model, and it raises the
  privilege/lifetime risk.

## Files

- `scripts/build-runtime.sh --host adb` -- the shell/ADB runtime
- `scripts/build-prefix.sh --host adb` -- the prefix profile
- `scripts/adb-deploy.sh` -- push, extract, check, boot
- `scripts/adb-shell.sh` -- `dn-adb`, the guarded shell wrapper
- `scripts/prefix/init.sh` -- the uid reconcile
- the two gates and the app-only answers:
  [`../reference/seccomp-gates.md`](../reference/seccomp-gates.md),
  [`contract.md`](contract.md).
