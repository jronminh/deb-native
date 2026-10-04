# Findings: software installed outside dpkg -- a second, untranslated install path (2026-10-04)

<!-- template: templates/docs.template.md -->

**Impact: Open gap.** deb-native translates a binary at **install time through
the apt/dpkg pipeline** (interpreter -> the prefix loader, maintainer scripts
through `dn-shell`, paths through the shim). Every other common way software
arrives -- `curl | bash`, language package managers, tarballs -- bypasses that
pipeline, so the resulting ELF is never translated and assumes a loader/FHS
that does not exist on Android. Surfaced by installing opencode's official
installer inside the prefix.

## Contents

- [The gap](#the-gap)
- [The common install paths](#the-common-install-paths)
- [Case study: opencode](#case-study-opencode)
- [Why the shim cannot help](#why-the-shim-cannot-help)
- [What it implies](#what-it-implies)

## The gap

There are two delivery routes, and only one is translated:

- **Route 1 -- distro packages (`.deb` via apt/dpkg).** Handled: the package's
  ELF gets its `PT_INTERP` rewritten to the prefix loader at install, its
  maintainer scripts run under `dn-shell`, and the `LD_PRELOAD` shim covers
  hardcoded `/usr /etc /var /opt` at run time.
- **Route 2 -- everything else.** `curl | bash` installers, `npm`/`pip`/
  `cargo binstall`, prebuilt tarballs: an **untranslated** ELF lands in
  `$HOME/...`, still pointing at the vendor's `PT_INTERP`
  (`/lib/ld-linux-aarch64.so.1`) and its FHS assumptions. Nothing in
  deb-native adopts it.

The axis that matters is **who produces the final ELF**: if the prefix does
(source build, or an install that runs the translator), its loader and paths
are right; if a third party ships a prebuilt binary, they are the vendor's and
must be adopted.

## The common install paths

| Install path | Delivers | deb-native status |
| --- | --- | --- |
| Distro package (`.deb`, apt/dpkg) | package ELF + maintainer scripts | **Handled** -- translate at install |
| Vendor shell installer (`curl \| bash`) | prebuilt ELF into `~/.tool` / `/usr/local/bin`, edits rc | **Gap** -- never translated (opencode, rustup, nvm, deno/bun, tailscale, ...) |
| Language pkg managers, prebuilt | downloaded ELF/`.so` (npm `esbuild`/`sharp`, `pip`/`uv` wheels, `cargo binstall`) | **Gap** -- same problem, just under a package manager |
| Language pkg managers, compiled | compiled in place (`go install`, `cargo install`, `gem`, `pip -e`) | **Works** if the prefix has the toolchain -- links the prefix loader |
| Prebuilt archive / single-file binary | raw ELF in a `.tar.gz` / standalone | **Depends** -- static (Go/Rust-musl) runs; dynamic glibc is the Gap |
| AppImage | ELF + bundled loader + squashfs via FUSE | **Out** -- needs FUSE + its own loader; Android has no FUSE for apps |
| Build from source (`./configure && make`) | object files linked against prefix libs | **Works** with the prefix toolchain (`gcc-glibc-dev.md`) |
| Containers (Docker/OCI), snap, flatpak | images / namespaces / loop mounts | **Out** -- no user namespaces, no daemon, no loop (unrooted) |
| Termux package (`pkg`) | Bionic packages into `$PREFIX` | Host layer, by design -- not the Debian userland |

So the "popular install paths" collapse into one question: **does the final
ELF come from the prefix (fine) or from a third party as a prebuilt (needs
adoption)?**

## Case study: opencode

The official installer (`opencode.ai/v2/install`) is a clean example:

- It detects the target from the **ambient** environment -- `uname -s`/`-m`
  (`linux-arm64`) and musl via `/etc/alpine-release` or `ldd --version`. It has
  no concept of a prefix.
- It downloads the npm tarball `@opencode/cli-linux-arm64`, extracts
  `package/bin/opencode`, and **`mv`s it to `~/.opencode/bin/opencode`** --
  no `patchelf`, no loader setup. It then writes
  `export PATH=$HOME/.opencode/bin:$PATH` into `~/.bashrc`.

Run inside `dn-shell`, detection sees **Debian/glibc** (the shim redirects
`/etc`; `ldd` is the prefix's) and fetches the glibc build -- which is the
right artifact for a Debian-minded prefix. But its `PT_INTERP` is
`/lib/ld-linux-aarch64.so.1`, absent from Android's real root, so `execve`
fails before anything else. And the `~/.bashrc` PATH edit does nothing in the
prefix: the prefix login shell is `bash -l`, which reads `~/.profile`, not
`~/.bashrc`.

## Why the shim cannot help

`PT_INTERP` is resolved by the **kernel**, before any user space runs. The
`LD_PRELOAD` shim loads *after* the interpreter, so it can never fix a bad
interpreter -- that is a kernel-level wall. This is exactly why the `.deb`
pipeline rewrites `PT_INTERP` at install; a third-party binary skips that
step, and no amount of shim coverage reaches it.

## What it implies

The missing primitive is an **adopt step for non-dpkg software**, generalizing
what the `.deb` pipeline already does. Two shapes, to weigh:

- **Translate the artifact** -- `patchelf --set-interpreter <prefix>/usr/lib/
  aarch64-linux-gnu/ld-linux-aarch64.so.1` (+ rpath). Works for ordinary ELF;
  risky for appended-data binaries like Bun's (section surgery can corrupt
  them).
- **Wrap the launch** -- leave the binary untouched and exec the prefix loader
  explicitly (`<prefix>/lib/ld-linux-aarch64.so.1 --library-path <prefix libs>
  <binary>`), the glibc-runner trick. This is what the opencode wrapper does
  and what the Termux musl launcher does for its own loader. Safe, but every
  adopted tool needs a wrapper and its env quirks.

Either way the open questions are the same and worth deciding before building:
**which environment the installer should run in** (inside `dn-shell`, to pick
the glibc build and stay "Debian", then adopt -- or on the host?), and **how an
adopted tool is registered on the prefix PATH/env** when the installer only
edits host rc files the prefix login never reads.
