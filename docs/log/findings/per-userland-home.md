# Findings: per-userland sparse home -- isolating program state from $HOME (2026-10-04)

<!-- template: templates/docs.template.md -->

**Impact: Open gap.** Sketch, no code yet. Sharing one `$HOME` between Termux
and the prefix (and between prefixes) makes their program files collide; this
proposes giving each prefix a **sparse home** for its own program state, while
`/home` stays the user's data home.

## Contents

- [Problem](#problem)
- [Design](#design)
- [What lives where](#what-lives-where)
- [Mechanism](#mechanism)
- [Recursion safety](#recursion-safety)
- [Open questions](#open-questions)

## Problem

One `$HOME` holds both worlds' program files and the user's data:

- Termux (Bionic) and the prefix (glibc) read/write the same `~/.bashrc`,
  `~/.profile`, `~/.config/*`, `~/.cache/*`, `~/.local/*`; two builds of the
  same tool collide on `~/.config/tool`. Two prefixes add a third writer.
- Login shells differ (`/etc/profile` Bionic vs Debian), so a shared dotfile
  breaks one side -- which is why the prefix's PATH had to move to
  `$DN/etc/profile.d/`.
- Placing anything self-referential under `$HOME` risks symlink recursion (the
  existing `$DN/root -> $HOME` landmine).

The file's *kind* is the real axis: **program state** (config/cache/installed,
machine-generated, disposable) vs **user data** (projects/documents, the user's).

## Design

Give each prefix a sparse home for program state; keep `/home` for user data.

```
/data/data/com.termux/files/.dn/<name>/        # prefix's HOME (program state only)
    .config/  .cache/  .local/  .npm/  .cargo/  .opencode/  .bashrc  .profile ...
    Projects   -> /data/data/com.termux/files/home/Projects    # leaf links to user data
    Downloads  -> /data/data/com.termux/files/home/Downloads
    Documents  -> /data/data/com.termux/files/home/Documents
```

`<name>` = the prefix's basename (already the registry key). `dn-shell` sets
`HOME=/data/data/com.termux/files/.dn/<name>` for the prefix session; `/home`
stays the Termux/user-data home and holds **no** prefix program files.

## What lives where

Defaults, meant to be configurable per prefix:

- **Isolated** (program state -> `.dn`): `.config`, `.cache`, `.local/share`,
  `.local/state`, `.npm`, `.cargo`, `.opencode`, `.bash_history`, and the
  login dotfiles `.bashrc`/`.profile`.
- **Shared** (user data -> leaf symlink to `/home`): `Projects`, `Documents`,
  `Downloads`, `Pictures`, `Music`, `Videos`, `Desktop`.
- **Identity** (a judgment call): `.ssh`, `.gnupg`, `.gitconfig` -- often
  *wanted* shared across worlds; either leaf-link (shared) or isolate.
- **`$HOME/.local/bin`**: keep the **real** home's on `PATH` (it holds the
  host-layer commands `dn-*` and a wrapper like `opencode`), or leaf-link it.

## Mechanism

- `dn-shell` (`native/dn-launch.c`): read the real `$HOME`, compute
  `.dn/<basename(instdir)>`, `mkdir -p` it, then `setenv("HOME", ...)` before
  exec'ing the prefix bash. Program writes then land in `.dn` by construction.
- Keep the **real** home path for the shared `$HOME/.local/bin` on `PATH`.
- Preserve the real `$HOME` for Bionic children (the `termux-shell` door),
  the same way the inherited termux-exec preload is stashed.
- The login selector `.dn-login` runs **before** `dn-shell` and keeps using
  the real `$HOME`; only the prefix sees the sparse home.

## Recursion safety

The rule is about **placement and leaf-ness**, not the link count:

- Never symlink a directory that (transitively) **contains `.dn`**, and never
  `$HOME` itself. Link **leaves** whose targets do not contain `.dn`.
- Prefer `.dn` **outside `$HOME`** (a sibling, like the prefix already is).
  Then even a `data -> $HOME` link is safe, because `$HOME` does not contain
  `.dn`.
- The danger is only for link-following tools: `find -L`, `du -L`, `rsync -L`,
  `cp -rL`, `tar -h`, `rg --follow`, LSP/IDE indexing, file pickers.

## Open questions

- The default isolate/share split, and where a per-prefix config for it lives.
- Migrating existing prefixes (their program files are in `/home` today).
- `dn-*` tooling to open/sync/inspect the `.dn` home.
- Interaction with `$DN/root -> $HOME` (the prefix already symlinks `/root` to
  the real home; a sparse `HOME` should keep that coherent).
