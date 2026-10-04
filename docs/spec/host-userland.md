# Host and userland: one interface over two roots

<!-- template: templates/docs.template.md -->

deb-native runs a Debian **userland** inside Termux, which is the
**host**. They are two separate filesystem roots (`$DN` and `$PREFIX`)
with **no containment boundary** between them — no namespaces, no chroot,
one shared `$HOME` and process tree. This doc fixes the interface model
that keeps that duality coherent: the userland is the default session, and
the host is reached through one explicit, nested command. Installed by
`scripts/runtime/make-shell-interface.sh`.

## Contents

- [The two worlds](#the-two-worlds)
- [Why the boundary lives in the interface](#why-the-boundary-lives-in-the-interface)
- [The default session is the userland](#the-default-session-is-the-userland)
- [Crossing to the host: `termux-shell`](#crossing-to-the-host-termux-shell)
- [Crossing back: `dn-shell`](#crossing-back-dn-shell)
- [Command set](#command-set)
- [Environment rules](#environment-rules)
- [Entry mechanism and recursion](#entry-mechanism-and-recursion)
- [Prompts and the welcome](#prompts-and-the-welcome)
- [What this removes](#what-this-removes)
- [What stays](#what-stays)
- [Fallback and reversibility](#fallback-and-reversibility)
- [Open items](#open-items)

## Related docs

- [`design.md`](design.md) — the mechanism end to end; this doc only
  defines the interactive interface over it.
- [`install-flow.md`](install-flow.md) — the bootstrap that produces the
  userland, and where the interface is installed.
- [`package-lifecycle.md`](package-lifecycle.md) — what runs inside the
  userland per package.
- [`dn-glibc-prefix.md`](dn-glibc-prefix.md) — the prefix itself, its
  loader, and why it reaches the host only through the tracer for
  static/raw-syscall programs.
- [`tracer.md`](tracer/tracer.md) / [`syscall-boundary.md`](../reference/syscall-boundary.md)
  — the host-side machinery the userland still needs.

## The two worlds

| | **Host** (Termux + Android) | **Userland** (`$DN`, Debian) |
|---|---|---|
| Is | the app, the process tree, the runtime, the plumbing | the Debian system you work in |
| Provides | terminal/session; Bionic runtime; the `$PREFIX/glibc` side-install; Android-legal syscalls; network/DNS/storage; `pkg` and Termux tooling | Debian packages, `apt`/`dpkg` + database; the project's own glibc + loader + shim; toolchains; the `/usr /etc /var /opt` view |
| Role | kernel + init + system tools | guest rootfs |
| Removable | no — nothing runs without it | yes — delete the prefix and Termux is untouched |

`dn-run` and `dn-trace` are **host-layer** binaries even though they live
in the prefix: they are Bionic builds (Termux clang) that must run before
any glibc environment exists. The shim, the loader and glibc are
**userland**. This split is the key to classifying any piece.

## Why the boundary lives in the interface

A VM or container enforces host/userland with namespaces; here there is
no boundary to enforce — the userland runs as ordinary processes in the
host's tree, sharing `/proc`, `$HOME` and the network, and its "root" is
only a path overlay (shim + loader + tracer). Since the boundary cannot
be *built*, the only way to a coherent experience is to make it
*explicit*: the two roots stay, but the interface shows one world at a
time and makes crossing a deliberate act.

The old interface presented both at once — one Termux shell with the
launcher directory prepended to `PATH` and `apt`/`dpkg` aliased to the
prefix — so `apt`, `python`, `git` were ambiguous and the resolution
leaked as PATH ordering and aliases.

## The default session is the userland

Termux's login shell is pointed at a small wrapper outside the prefix,
which execs `dn-shell`:

```
Termux app
  └─ $PREFIX/bin/login
       └─ ~/.termux/shell   →  ~/.dn-login   (outside the prefix, with fallback)
            └─ dn-shell          ← DEFAULT = userland (Debian)
```

`dn-shell` (`dn-launch.c`) builds the userland view: `LD_PRELOAD` = the
shim, `DN_INSTDIR`, and a PATH that is

```
$DN/usr/lib/deb-native/priv            (the privilege layer, first)
$DN/usr/lib/deb-native/bin             (the launchers: tracer routing)
$DN/usr/sbin : $DN/usr/bin : $DN/sbin : $DN/bin : $DN/usr/games
$PREFIX/glibc/bin : $PREFIX/bin        (Termux's glibc coreutils, then Bionic)
```

The launchers are on the userland `PATH` so a program that needs the
tracer is routed there rather than run directly out of `usr/bin`
(`make-launchers.sh`). The prefix's directories come first, so `apt`,
`dpkg` and every installed program run by name, with no `~/.bashrc`
activation, no `command_not_found` trick, and no aliases.

## Crossing to the host: `termux-shell`

`termux-shell` (`$PREFIX/bin/termux-shell`) opens a **nested** host shell
as a child process (not `exec`), so leaving it returns to `dn-shell` with
cwd and state intact:

```
dn-shell (root, userland)
  └─ termux-shell
       └─ $PREFIX/bin/bash   (host PATH, no shim)
            └─ exit → back to dn-shell
```

The child must be a **clean** host environment, because the userland's
shim and `DN_INSTDIR` are session-global:

```sh
unset LD_PRELOAD DN_INSTDIR DN_BIONIC_PRELOAD DN_REDIRECT_PREFIXES DN_ID PROMPT_COMMAND
export PATH="$PREFIX/bin:$PREFIX/bin/applets"
export SHELL="$PREFIX/bin/bash"
"$PREFIX/bin/bash" -i        # a child, not exec
```

It deliberately does **not** call `$PREFIX/bin/login` (that would
reselect `~/.termux/shell` and recurse).

## Crossing back: `dn-shell`

From a host shell, `dn-shell` (`$PREFIX/bin/dn-shell`) enters the userland
again — the explicit reverse direction, so the boundary is crossable both
ways. It is a thin wrapper that execs `$DN/usr/bin/dn-shell`.

## Command set

| command | from → to | what |
|---|---|---|
| `termux-shell` | userland → host | nested, clean host shell |
| `dn-shell` | host → userland | enter the userland session |
| `pkg` | host only | Termux's package manager |
| `apt`, `dpkg` | userland only | the prefix's Debian packages |

`apt`/`dpkg` are the userland's by PATH; a `pkg` guard in the prefix
(`$DN/usr/lib/deb-native/priv/pkg`, first on the userland PATH) refuses
inside the userland and points at `termux-shell`. Host package management
is only ever spelled with `pkg` (or `termux-apt`/`termux-dpkg` from the
launcher directory). No word resolves differently by context.

## Environment rules

- The shim, `DN_INSTDIR`, `DN_BIONIC_PRELOAD`, `DN_REDIRECT_PREFIXES` and
  `DN_ID` are **userland-session state**; `termux-shell` scrubs them before
  the host shell starts.
- A host tool reached by bare name from the userland does not exist;
  crossing is via `termux-shell`.
- Non-interactive host work uses `termux-shell -c '...'`.
- The userland login bash sources the prefix's `/etc/profile` (Debian's),
  which sets `PS1` when one is present; the red userland prompt is set by
  `PROMPT_COMMAND` so an unconditional `PS1=` there cannot win.

## Entry mechanism and recursion

Auto-entry is done through `~/.termux/shell` (a symlink to `~/.dn-login`,
executed once by `$PREFIX/bin/login`), **not** by an `exec dn-shell` line
in `.bashrc`: a nested host bash would source `.bashrc` and re-exec
`dn-shell` forever. Keeping `.bashrc`/`.bash_profile` free of the
auto-entry avoids that entirely. `make-shell-interface.sh` also strips the
old managed `# deb-native` block from `~/.bashrc`, since a host shell
sources it and the interface must leave the host clean.

## Prompts and the welcome

Each world marks itself by prompt (and each sets it in its own entry):

- userland: red `~ # ` — the prompt uses a literal `#`; the userland is
  always fake-root.
- host: green `~ $ ` — `termux-shell` sets `PS1` and clears
  `PROMPT_COMMAND`; the host is a normal (non-root) user.

At login, `~/.termux/motd.sh` prints the dn-shell welcome in place of
Termux's static `/etc/motd` (Termux's `login` prefers that file when it
exists): what dn-shell is, the `Docs`/`Contribute` links, and the
`apt`/`termux-shell` split.

## What this removes

- The `~/.bashrc` activation (launcher dir on `PATH`, `apt`/`dpkg`
  aliases): the launcher dir is now baked into the userland PATH by
  `dn-launch.c`, and there are no aliases.
- The interactive launcher directory *as an activation mechanism* — the
  directory stays (tracer routing), but nothing in `~/.bashrc` touches it.
- The escape-hatch sprawl: it collapses into `termux-shell` and the `pkg`
  guard.

## What stays

- Routing of static / raw-syscall / Bionic programs through
  `dn-run`/`dn-trace`, via the launcher directory on the userland PATH —
  the loader covers glibc-dynamic ELFs only.
- The shim, the fused loader, the translate/launcher hooks.
- The bootstrap's package work; only the *interactive* interface changed.

## Fallback and reversibility

- `~/.termux/shell` points at a wrapper **outside** the prefix, so a
  missing or broken prefix falls back to the host shell rather than
  locking the user out:

  ```sh
  [ -x "$DN/usr/bin/dn-shell" ] && exec "$DN/usr/bin/dn-shell" "$@" \
    || exec "$PREFIX/bin/bash" "$@"
  ```

- Removing `~/.termux/shell` restores Termux's default shell. Full undo:

  ```sh
  rm -f ~/.termux/shell ~/.dn-login ~/.termux/motd.sh \
        "$PREFIX/bin/termux-shell" "$PREFIX/bin/dn-shell"
  ```

- The change is confined to `~/.termux/shell`, `~/.dn-login`,
  `~/.termux/motd.sh`, two files in `$PREFIX/bin`, and the guard inside
  the prefix — so "Termux untouched after uninstall" still holds once the
  interface is undone.

## Open items

- The launcher directory is still generated (retiring it in favour of a
  runtime dispatcher is separate; `TODO.md`).
- `termux-shell` leaves `termux-exec` (`$PREFIX/lib/libtermux-exec-ld-preload.so`)
  unset for the host shell; restore it if a host tool ever needs it.
- Terminal-title markers for each world.
