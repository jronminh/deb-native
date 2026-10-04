# One host, many userlands: one interface over sibling roots

<!-- template: templates/docs.template.md -->

deb-native runs a Debian **userland** beside Termux's own in one Android
app. They are two software roots (`$TP` = Termux's `usr/`, `$DN` = ours) on
the same **host** — Android's kernel, Bionic and app sandbox — with **no
containment boundary** between them: no namespaces, no chroot, one shared
`$HOME` and process tree. Neither userland outranks the other; they are
siblings, and only the *bootstrap* step borrows the Termux side (a
build-time dependency, not a hierarchy — `TODO.md` "0.7.0" is about making
runtime need nothing from it). This doc fixes the interface model that keeps
the siblings coherent: the Debian userland is the default session, and the
Termux userland is reached through one explicit, nested command. Installed
by `scripts/runtime/make-shell-interface.sh`.

## Contents

- [The host and its userlands](#the-host-and-its-userlands)
- [Why the crossing lives in the interface](#why-the-crossing-lives-in-the-interface)
- [The default session is the Debian userland](#the-default-session-is-the-debian-userland)
- [Crossing to the Termux userland: `termux-shell`](#crossing-to-the-termux-userland-termux-shell)
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
  — the host-side (Android/Bionic) machinery the userland still needs.

## The host and its userlands

| | **Host** (Android) | **Termux userland** (`$TP`) | **Debian userland** (`$DN`) |
|---|---|---|---|
| Is | the kernel, Bionic runtime, app sandbox, process tree | the Termux install | the Debian tree this project installs |
| Provides | syscalls, SELinux/seccomp, network/DNS/storage, the shared `$HOME` | `pkg` and Termux tooling; the parent login session; build-time material | Debian packages, `apt`/`dpkg` + database; its own glibc + loader + shim; toolchains; the `/usr /etc /var /opt` view |
| Rank | the machine | sibling userland | sibling userland |
| Removable | no | no, while Termux is installed | yes — delete `$DN` and the rest is untouched |

`dn-run` and `dn-trace` are **host-layer** binaries even though they live
in the prefix: they are Bionic builds (Termux clang) that must run before
any glibc environment exists. The shim, the loader and glibc are
**userland**. This split is the key to classifying any piece — a
runtime-layer split, not a rank.

## Why the crossing lives in the interface

A VM or container enforces userland isolation with namespaces; here there
is no boundary to enforce — both userlands run as ordinary processes on the
host, sharing `/proc`, `$HOME` and the network, and the Debian one's "root"
is only a path overlay (shim + loader + tracer). Since the boundary cannot
be *built*, the only way to a coherent experience is to make it
*explicit*: the roots stay, but the interface shows one userland at a time
and makes crossing a deliberate act.

The old interface presented both at once — one Termux shell with the
launcher directory prepended to `PATH` and `apt`/`dpkg` aliased to the
prefix — so `apt`, `python`, `git` were ambiguous and the resolution leaked
as PATH ordering and aliases.

## The default session is the Debian userland

Termux's login shell is pointed at a small wrapper outside the prefix,
which execs `dn-shell`:

```
Termux app
  └─ $PREFIX/bin/login
       └─ ~/.termux/shell   →  ~/.dn-login   (outside the prefix, with fallback)
            └─ dn-shell          ← DEFAULT = Debian userland
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

## Crossing to the Termux userland: `termux-shell`

`termux-shell` (`$PREFIX/bin/termux-shell`) opens a **nested** Termux shell
as a child process (not `exec`), so leaving it returns to `dn-shell` with
cwd and state intact:

```
dn-shell (Debian userland)
  └─ termux-shell
       └─ $PREFIX/bin/bash   (Termux PATH, no shim)
            └─ exit → back to dn-shell
```

The child must be a **clean** Termux environment, because the Debian
userland's shim and `DN_INSTDIR` are session-global:

```sh
unset LD_PRELOAD DN_INSTDIR DN_BIONIC_PRELOAD DN_REDIRECT_PREFIXES DN_ID PROMPT_COMMAND
export PATH="$PREFIX/bin:$PREFIX/bin/applets"
export SHELL="$PREFIX/bin/bash"
"$PREFIX/bin/bash" -i        # a child, not exec
```

It deliberately does **not** call `$PREFIX/bin/login` (that would
reselect `~/.termux/shell` and recurse).

## Crossing back: `dn-shell`

From a Termux shell, `dn-shell` (`$PREFIX/bin/dn-shell`) enters the Debian
userland again — the explicit reverse direction, so the crossing works both
ways. It is a thin wrapper that execs `$DN/usr/bin/dn-shell`.

## Command set

| command | from → to | what |
|---|---|---|
| `termux-shell` | Debian userland → Termux userland | nested, clean Termux shell |
| `dn-shell` | Termux userland → Debian userland | enter the Debian session |
| `pkg` | Termux userland only | Termux's package manager |
| `apt`, `dpkg` | Debian userland only | the prefix's Debian packages |

`apt`/`dpkg` are the userland's by PATH; a `pkg` guard in the prefix
(`$DN/usr/lib/deb-native/priv/pkg`, first on the userland PATH) refuses
inside the userland and points at `termux-shell`. Termux package
management is only ever spelled with `pkg` (or `termux-apt`/`termux-dpkg`
from the launcher directory). No word resolves differently by context.

## Environment rules

- The shim, `DN_INSTDIR`, `DN_BIONIC_PRELOAD`, `DN_REDIRECT_PREFIXES` and
  `DN_ID` are **Debian-userland session state**; `termux-shell` scrubs them
  before the Termux shell starts.
- A Termux tool reached by bare name from the Debian userland does not
  exist; crossing is via `termux-shell`.
- Non-interactive Termux work uses `termux-shell -c '...'`.
- The Debian userland login bash sources the prefix's `/etc/profile`
  (Debian's), which sets `PS1` when one is present; the red userland prompt
  is set by `PROMPT_COMMAND` so an unconditional `PS1=` there cannot win.

## Entry mechanism and recursion

Auto-entry is done through `~/.termux/shell` (a symlink to `~/.dn-login`,
executed once by `$PREFIX/bin/login`), **not** by an `exec dn-shell` line
in `.bashrc`: a nested Termux bash would source `.bashrc` and re-exec
`dn-shell` forever. Keeping `.bashrc`/`.bash_profile` free of the
auto-entry avoids that entirely. `make-shell-interface.sh` also strips the
old managed `# deb-native` block from `~/.bashrc`, since a Termux shell
sources it and the interface must leave the Termux side clean.

## Prompts and the welcome

Each userland marks itself by prompt (and each sets it in its own entry):

- Debian userland: red `~ # ` — the prompt uses a literal `#`; the userland
  is always fake-root.
- Termux userland: green `~ $ ` — `termux-shell` sets `PS1` and clears
  `PROMPT_COMMAND`; it is a normal (non-root) user.

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
  missing or broken prefix falls back to the Termux shell rather than
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
  unset for the Termux shell; restore it if a Termux tool ever needs it.
- Terminal-title markers for each userland.
