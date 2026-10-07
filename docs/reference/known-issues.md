# Known issues

<!-- template: templates/docs.template.md -->

Confirmed breakages and limitations in the current tree — reproduced, not
merely suspected. Each entry says how to reproduce it, what happens, and the
known root cause or workaround. Future work lives in [`TODO.md`](../../TODO.md);
whole classes of "what the shim/tracer cannot reach" live in
[`syscall-boundary.md`](syscall-boundary.md),
[`../spec/shim/shim-coverage.md`](../spec/shim/shim-coverage.md) and
[`android-platform.md`](android-platform.md).

## Contents

- [Bugs with a reproduction](#bugs-with-a-reproduction)
- [Mitigated breakages](#mitigated-breakages)
- [Design limitations](#design-limitations)

## Related docs

- [`../spec/design.md`](../spec/design.md) — the path shim and the
  maintainer-script mechanism these issues hit.
- [`../spec/shim/shim-coverage.md`](../spec/shim/shim-coverage.md) — the
  path-shim failure modes.
- [`syscall-boundary.md`](syscall-boundary.md), [`android-platform.md`](android-platform.md)
  — what libc interposition cannot see, and the platform's enforcement gates.

## Bugs with a reproduction

### Maintainer scripts with a raw interpreter shebang

**Symptom.** `dpkg --configure` of a package whose `postinst` reads `/etc`
fails and leaves it `iF`:

```
sed: can't read /etc/ca-certificates.conf: No such file or directory
dpkg: error processing package ca-certificates (--configure):
 post-installation script subprocess returned error exit status 2
```

**Root cause.** A maintainer script whose shebang is a raw
`#!<prefix>/usr/bin/dash` is run by the kernel **without** the shim (the shim
is delivered via a launcher/`ld.so.preload`), so its `/etc`, `/usr` reads hit
the host's real root. The translator (`scripts/prefix/dn-translate-deb.sh`)
must rewrite every maintainer-script shebang, not a subset; a missed one keeps
the raw interpreter.

**Workaround.** Invoke the script through the interpreter with the shim in the
environment. The durable fix is to rewrite every shebang.

### An alternatives symlink points outside the prefix

**Symptom.** After installing a package that uses alternatives, the command is
not found: its symlink resolves outside the prefix, where nothing exists.

**Root cause.** `update-alternatives` joins its compiled-in absolute dirs onto
`DPKG_ROOT`, writing a link into the host's tree instead of the prefix's; the
`priv/update-alternatives` wrapper (forcing `--altdir`/`--admindir`) must be on
dpkg's maintainer PATH, and the links are made relative immediately
(`dn-fix-alternatives`).

### Python venv: the `pip` launcher fails

**Symptom.** A venv's `pip` script fails on any command that builds pip's
network session:

```
subprocess.CalledProcessError: Command '('uname', '-rs')' returned non-zero exit status 1.
```

**Root cause.** Not fully root-caused. Specific to invoking the installed
`pip` **launcher script** (a shebang script); `python3 -m pip` is unaffected.

**Workaround.** Always `python3 -m pip`, not `pip`/`pip3` directly.

### `pytest` dies with `Bad system call`

**Symptom.** `pytest` (even a trivial test) dies with `Bad system call`
(SIGSYS).

**Root cause.** A syscall in pytest's machinery is not in the shim's covered
set and not auto-routed to the tracer; not root-caused (same class as the
`ldconfig -r` SIGSYS).

**Workaround.** Run under the tracer: `dn-trace -- python3 -m pytest`.

### `ctypes`/`cffi` `dlopen` by bare name fails

**Symptom.** `ctypes.CDLL("libfoo.so")` / `cffi` opening a **self-built**
library by bare name fails `cannot open shared object file`.

**Root cause.** `dlopen` consults `LD_LIBRARY_PATH` and the cache; a
self-built library outside those is not found.

**Workaround.** Set `LD_LIBRARY_PATH` (or use an absolute path) before
launching.

### A binary with a literal `/lib/ld-linux-aarch64.so.1`

**Symptom.** A binary the prefix's own `gcc` produced fails
`cannot execute: required file not found` when its `PT_INTERP` is the literal
`/lib/ld-linux-aarch64.so.1`.

**Root cause.** The gcc `specs` file is missing or stale. Fix: rerun
`dn-fix-gcc-specs.sh` (wired into the post hook).

### `dn-trace` fails: `libtalloc.so.2` not found

**Symptom.** A program that `dn-run` routes to `dn-trace` (seen with
`ssh-keygen`, `sshd` and `dropbear` adopted from a `.deb`) dies at once:

```
dn-trace: error while loading shared libraries: libtalloc.so.2: cannot open shared object file
```

**Root cause.** `dn-trace` links `libtalloc`, and `core-deb` does not install
`libtalloc2`.

**Workaround.** `apt install libtalloc2`; the same programs then run.

### Stale apt hook paths after a checkout moves

**Symptom.** `apt`/`dpkg` break on an existing prefix after the checkout
directory is moved or renamed.

**Root cause.** The prefix's `apt.conf` bakes absolute hook paths
(`DPkg::Pre-Install-Pkgs`, `DPkg::Post-Invoke`, ...) once; nothing regenerates
them. Tracked in [`TODO.md`](../../TODO.md).

## Mitigated breakages

The host workarounds are installed for you in
`<prefix>/usr/lib/deb-native/priv/`.

- **`update-alternatives` / `dpkg-divert` doubled-path.** Under `DPKG_ROOT`
  they join it onto their compiled-in absolute dirs; the `priv/` wrappers
  force `--altdir`/`--admindir`/`--instdir` and links are made relative.
- **`chroot` is killed by the platform's seccomp** (SIGSYS) — a postinst doing
  `chroot "$DPKG_ROOT" ...` would fail and wedge later `apt` runs; the
  `priv/chroot` wrapper runs the command directly (only the prefix root is
  accepted).
- **`getent`** for the account databases reaches the host's glibc NSS
  (libc-internal); the `priv/getent` wrapper answers
  `passwd`/`group`/`shadow`/`gshadow` from the prefix's own files.
- **`ldconfig` is a no-op** in the priv layer: `libc-bin`'s postinst calls
  `ldconfig -r "$DPKG_ROOT/"`, which cannot work unprivileged. No `ld.so.cache`
  ships ([`../spec/dn-glibc-prefix.md`](../spec/dn-glibc-prefix.md)); the loader derives its
  dirs from the live prefix, so a missing cache is non-fatal.

## Design limitations

By design, not bugs; see the linked specs.

- **Paths not redirected:** `/tmp`, `/run`, `/proc`, `/sys`, `/lib64` are left
  to the host (`shim/shim-coverage.md`). A missing
  `/tmp` is a common source of write/ENOENT failures.
- **No shim without `LD_PRELOAD`:** an emptied environment (`env -i`, setuid),
  an absolute-path invocation that bypasses the launchers, or a Bionic child
  all lose the shim and see the real root
  (`shim/shim-coverage.md`).
- **No init/service manager and no child reaper:** a package shipping a unit
  installs but the service does not run, and double-forked zombies accumulate
  (services are a roadmap item).
- **OpenSSH `sshd` cannot run.** A connection reaches pre-auth and then fails
  at the privilege-separation `chroot("/run/sshd")`: the syscall is blocked by
  Android's seccomp (`dn-trace warning: blocked syscall chroot (#51) denied by
  seccomp; returning ENOSYS`), and OpenSSH has no option to turn privilege
  separation off. `dropbear` is the SSH server: with a key in
  `~/.ssh/authorized_keys` it logs in to the prefix's own shell. It also needs
  the login user to have a shell in `/etc/passwd` that is listed in
  `/etc/shells`; `core-deb` ships neither file, the shim answers `getpwnam`
  with a synthesized entry whose shell is Termux's `login`, and `dropbear`
  rejects that as an invalid shell.
- **`uname` reports the platform**, and `os-release`/`lsb_release`/`systemd`/
  `dbus` may be absent, so programs branching on them can misbehave
  (`shim/shim-coverage.md`).
- **Two package managers share the home dir** when the host has its own
  (`~/.config`, `~/.cache`, `~/.local` collide).
- **`--force-architecture` for `arm64` vs `aarch64`** is still in use and
  flagged unsafe.
- **Perl version gap:** the package ABI a module is built for must match the
  prefix's `perl`.
- **`dn-adopt`'d self-updating programs** re-download a fresh, un-adopted
  binary on update; re-adopt it or disable the updater.
- **`base-files`' custom patch** is applied by exact-line `sed` and refuses
  loudly if upstream changes those lines.
- **Per-project builds are partly unverified:** `g++`/C++ is untested and
  `rustc`/`ghc` are unresearched.
