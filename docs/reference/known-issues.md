# Known issues

<!-- template: templates/docs.template.md -->

Confirmed breakages and limitations in the current tree — reproduced, not
merely suspected. This is a catalog of the current state, like the other
specs, and a hub for issues documented in more depth elsewhere: each entry
says how to reproduce the problem, what actually happens, and the known root
cause or workaround. Deeper tracking and future work live in
[`TODO.md`](../../TODO.md); whole classes of "what the shim/tracer cannot
reach" live in `runtime-failures.md`, `syscall-boundary.md` and
`android-platform.md`.

## Contents

- [Bugs with a reproduction](#bugs-with-a-reproduction)
  - [Maintainer scripts with a raw interpreter shebang](#maintainer-scripts-with-a-raw-interpreter-shebang)
  - [figlet's alternatives symlink points into Termux's tree](#figlets-alternatives-symlink-points-into-termuxs-tree)
  - [Python venv: the `pip` launcher fails](#python-venv-the-pip-launcher-fails)
  - [`pytest` dies with `Bad system call`](#pytest-dies-with-bad-system-call)
  - [`ctypes`/`cffi` `dlopen` by bare name fails](#ctypescffi-dlopen-by-bare-name-fails)
  - [gcc-linked binary with a literal `/lib/ld-linux-aarch64.so.1`](#gcc-linked-binary-with-a-literal-libld-linux-aarch64so1)
  - [Stale apt hook paths after the checkout moves](#stale-apt-hook-paths-after-the-checkout-moves)
- [Mitigated breakages](#mitigated-breakages)
- [Design limitations](#design-limitations)

## Related docs

- `design.md` — the path shim and the maintainer-script mechanism these
  issues hit.
- `install-flow.md` — the bootstrap where maintainer-script shebangs are
  rewritten.
- `runtime-failures.md` — the catalog of path-shim failure modes (paths not
  redirected, `LD_PRELOAD` lost, no services/init, ...).
- `syscall-boundary.md` / `android-platform.md` — what libc interposition
  cannot see, and the Android enforcement gates.
- `userlands.md` — the host/userland model (one host, sibling userlands);
  the shim is userland session state.

## Bugs with a reproduction

### Maintainer scripts with a raw interpreter shebang

**Symptom.** `dpkg --configure ca-certificates` (or an install/reinstall of
it) fails and leaves the package `iF`:

```
sed: can't read /etc/ca-certificates.conf: No such file or directory
dpkg: error processing package ca-certificates:arm64 (--configure):
 installed ca-certificates:arm64 package post-installation script subprocess returned error exit status 2
```

**Root cause.** Most maintainer scripts have their shebang rewritten to
`#!$INSTDIR/usr/bin/dn-sh` — the launcher, which sets `LD_PRELOAD` to the
path shim before the interpreter runs. A few are missed by that rewrite
(`patch-scripts-tree.sh`) and keep a raw `#!$INSTDIR/usr/bin/dash`. On this
prefix they are `ca-certificates`, `cpp`, `figlet`, `gcc` and
`libcrypt-dev` (5 of 22 `postinst`s).

When the kernel executes such a script, its interpreter runs **without** the
shim, so the script's `/etc`, `/usr`, ... reads hit Android's real root
instead of the prefix. Running the interpreter explicitly instead of via the
shebang does not have this problem. Minimal repro, in the userland:

```sh
cat > ~/t.sh <<'EOF'
#!/data/data/com.termux/files/deb-native/usr/bin/dash
sed -n 1p /etc/ca-certificates.conf
EOF
chmod 755 ~/t.sh
~/t.sh            # sed: can't read /etc/ca-certificates.conf  (rc=2)
dash ~/t.sh       # prints the file                            (rc=0)
```

**Workaround.** Invoke the missed script through the interpreter with the
shim in the environment (re-export `DN_INSTDIR` and `LD_PRELOAD` first). The
durable fix is to rewrite every maintainer-script shebang, not a subset.

### figlet's alternatives symlink points into Termux's tree

**Symptom.** After installing `figlet`, the command is not found: its
alternatives link resolves into Termux's tree, where nothing exists.

```
$ ls -l $DN/usr/bin/figlet
/.../usr/bin/figlet -> /data/data/com.termux/files/usr/etc/alternatives/figlet
```

**Root cause.** `figlet`'s postinst is one of the missed raw-shebang scripts
above. Its `update-alternatives` call then resolves to Termux's real one,
which writes the link against Termux's own `/etc/alternatives` rather than
the prefix's (`setup-runtime.sh`'s `priv/update-alternatives` wrapper, which
forces `--altdir`, is not on dpkg's maintainer PATH).

### Python venv: the `pip` launcher fails

**Symptom.** Calling a venv's `pip` script directly fails on any command
that builds pip's network session (`install`, `download`, even
`pip install --upgrade pip`):

```
subprocess.CalledProcessError: Command '('uname', '-rs')' returned non-zero exit status 1.
```

`pip._vendor.distro` shells out to `uname -rs` for the User-Agent string;
the same call run any other way succeeds.

**Root cause.** Not root-caused. Specific to invoking the installed
`.venv-dn/bin/pip` **launcher script** (a shebang script); candidate: how
shim/launcher resolution handles a script invoked by its own long translated
path rather than via `python3 -m`.

**Workaround.** Always `python3 -m pip`, never `pip`/`pip3` directly. Full
detail: [`../guides/python-venv.md`](../guides/python-venv.md).

### `pytest` dies with `Bad system call`

**Symptom.** `pytest` (even a trivial pure-Python test) dies with `Bad
system call` (SIGSYS). `-p no:cacheprovider`, `-p no:faulthandler` and `-s`
do not avoid it.

**Root cause.** Some syscall in pytest's collection/capture/cache machinery
is not in the shim's covered set and is not auto-routed to the tracer; not
root-caused (same class as the `ldconfig -r` SIGSYS).

**Workaround.** Run under the tracer:
`~/deb-native/tracer/dn-trace -- .venv-dn/bin/python3 -m pytest tests/`.
Full detail: [`../guides/python-venv.md`](../guides/python-venv.md).

### `ctypes`/`cffi` `dlopen` by bare name fails

**Symptom.** `ctypes.CDLL("libfoo.so")` / `cffi` opening a **self-built**
library by bare name fails `OSError: cannot open shared object file`.

**Root cause.** `dlopen` consults `LD_LIBRARY_PATH` and the cache; a
self-built library outside those is not found. Wheels with bundled
`-rpath $ORIGIN` deps are unaffected.

**Workaround.** Set `LD_LIBRARY_PATH` (or use an absolute path) before
launching. Full detail: [`../guides/python-venv.md`](../guides/python-venv.md).

### gcc-linked binary with a literal `/lib/ld-linux-aarch64.so.1`

**Symptom.** A binary produced by the prefix's own `gcc` fails
`cannot execute: required file not found` when its `PT_INTERP` is the
literal `/lib/ld-linux-aarch64.so.1` instead of the prefix's fused loader.

**Root cause.** The gcc `specs` file is missing or stale. Fix: rerun
`dn-fix-gcc-specs.sh`. Full detail:
[`../guides/gcc-glibc-dev.md`](../guides/gcc-glibc-dev.md).

### Stale apt hook paths after the checkout moves

**Symptom.** `apt`/`dpkg` break on an existing prefix after the checkout
directory is moved or renamed.

**Root cause.** `setup-apt-prefix.sh` writes absolute hook paths
(`DPkg::Pre-Install-Pkgs`, `DPkg::Post-Invoke`, ...) into the prefix's
`apt.conf` once; nothing regenerates them. Tracked in `TODO.md` (backlog).

## Mitigated breakages

These still need their workaround; it is just installed for you, in
`$INSTDIR/usr/lib/deb-native/priv/` (`setup-runtime.sh`).

- **`update-alternatives` / `dpkg-divert` doubled-path.** Under `DPKG_ROOT`
  they join it onto their compiled-in absolute dirs, writing links into
  Termux's tree; the `priv/` wrappers force `--altdir`/`--admindir`/`--instdir`
  and links are made relative immediately (`dn-fix-alternatives.sh`).
- **`chroot` is killed by seccomp** (SIGSYS) — packages whose postinst does
  `chroot "$DPKG_ROOT" ...` would fail and wedge later `apt` runs; the
  `priv/chroot` wrapper runs the command directly (only the prefix root is
  accepted).
- **`getent`** for the account databases reaches Termux glibc's NSS, which
  reads `$PREFIX/glibc/etc` (libc-internal, beyond the shim); the
  `priv/getent` wrapper answers `passwd`/`group`/`shadow`/`gshadow` from the
  prefix's own files.
- **`ldconfig` is a no-op** in the priv layer: `libc-bin`'s postinst calls
  `ldconfig -r "$DPKG_ROOT/"`, which cannot work unprivileged. The real cache
  rebuild is `dn-install-glibc.sh`'s job, by full path.
- **The shipped `ldconfig` is static**, so it cannot derive the prefix
  (`__dn_prefix_get` is NULL, `__dn_build` yields empty paths). The bootstrap
  runs it under the tracer with explicit `-C`/`-f` and ignores its exit
  status; a missing `ld.so.cache` is non-fatal
  ([`deploy.md`](../spec/deploy.md)).

## Design limitations

By design, not bugs; see the linked specs rather than treating them as
regressions.

- **Paths not redirected:** `/tmp`, `/run`, `/proc`, `/sys`, `/lib64` are
  left to Android (`runtime-failures.md`, `shim-coverage.md`). `/tmp` being
  absent is a common source of write/ENOENT failures.
- **No shim without `LD_PRELOAD`:** an emptied environment (`env -i`,
  setuid), an absolute-path invocation that bypasses the launchers, or a
  Bionic child all lose the shim and see the real root
  (`runtime-failures.md`, "Highest risk #1").
- **No init/service manager and no child reaper:** packages shipping a
  systemd unit install but the service does not run, and double-forked
  zombies accumulate (`runtime-failures.md`; services are a roadmap item).
- **`uname` reports `Android`**, and `os-release`/`lsb_release`/`systemd`/
  `dbus` are absent, so programs branching on them can misbehave
  (`runtime-failures.md`).
- **Two package managers share `$HOME`:** Termux and the userland collide in
  `~/.config`, `~/.cache`, `~/.local` (`runtime-failures.md`).
- **`--force-architecture` for `arm64` vs `aarch64`** is still in use and
  flagged unsafe; `dpkg --print-architecture` answers `aarch64` inside the
  prefix (`design.md`, `TODO.md`).
- **Perl version gap:** the embedded `perl` is Termux's 5.42 while trixie's
  `perl` builds modules for 5.40 (`TODO.md`).
- **`dn-adopt`'d self-updating programs** re-download a fresh, un-adopted
  binary on update; re-adopt it or disable the updater (`dn-adopt.sh`).
- **`base-files`' custom patch** is applied by exact-line `sed` and refuses
  loudly if upstream changes those lines, so a newer `base-files` breaks the
  custom fix (`custom/base-files.sh`).
- **Tailscale native is not started:** static Go binaries cannot be
  shim-redirected, `/dev/net/tun` is root-only, and there is no systemd;
  run it via the tracer or with explicit `--state`/`--socket`
  ([`../guides/tailscale.md`](../guides/tailscale.md)).
- **Per-project builds are partly unverified:** `g++`/C++ is untested and
  `rustc`/`ghc` are unresearched ([`../guides/gcc-glibc-dev.md`](../guides/gcc-glibc-dev.md)).
