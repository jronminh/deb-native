# Install flow — what happens, in order

> Template: [`templates/docs.template.md`](../../templates/docs.template.md)
> (fix the relative path to match this file's depth). Read a doc's
> summary and table of contents below before its sections, and read
> its directory's own `README.md` first to confirm this is the right
> doc to open. Create a new doc, instead of extending an existing
> one, when the content is a distinct kind of writing -- a new spec
> topic, a new one-off investigation, or a new guide -- not just a
> long addition to what a doc already covers.

From nothing to "any `apt-get install` works". Grounded in the scripts; the
mechanisms each step installs are documented in
[`path-shim.md`](path-shim.md) (shim and maintainer-script layers),
[`tracer.md`](tracer.md) (the tracer),
[`syscall-boundary.md`](syscall-boundary.md) (what each layer reaches), and
[`bind-only.md`](bind-only.md) (the tracer's path fast path).

## Contents

- [The sequence](#the-sequence)
- [Three clarifications](#three-clarifications)
- [The apt/dpkg hooks](#the-aptdpkg-hooks)
- [End-to-end test](#end-to-end-test)

## Related docs

- [`path-shim.md`](path-shim.md) — the shim and maintainer-script
  layers this flow installs.
- [`tracer.md`](tracer.md) — the tracer wired in during this flow.
- [`syscall-boundary.md`](syscall-boundary.md) — what each layer
  installed here actually reaches.
- [`bind-only.md`](bind-only.md) — the tracer's path fast path.
- [`classic-design.md`](classic-design.md) — the pre-0.2.0 install
  pipeline this flow superseded.

## The sequence

This is `setup-apt-prefix.sh`'s own bootstrap, stage by stage (its `mark
stage` lines name each one) — not a per-install sequence; a prefix only
goes through this once.

1. **Runtime first, before anything else.** `setup-apt-prefix.sh` calls
   `setup-runtime.sh` directly, before the prefix's own dpkg database
   even exists. It builds/installs:
   - the **shim** `path-redirect.so` (glibc layer);
   - **`dn-run`**, the launch classifier, and **`dn-shell`/`dn-perl`**;
   - the **tracer** as `dn-trace` when `make` and `libtalloc` are present;
     without it, `dn-run` warns and runs those programs untranslated (no
     fallback to Termux's `proot`).
   Ordering is load-bearing: maintainer scripts run during dpkg's `--unpack`,
   so the interpreter and shim must already exist before the base is
   unpacked below.

2. **Stand-ins, then the base, downloaded and translated as a batch.**
   `dn-standins.sh` installs the `libc6`/`dpkg`/`apt` stand-ins into a
   throwaway apt config; `apt-get --download-only` then resolves and
   fetches the base package set. Each downloaded `.deb` is translated in
   parallel by `dn-translate-deb.sh` directly (the prefix's own apt hooks
   don't exist yet at this point in the bootstrap) — `Architecture: all`
   -> `arm64`, ELF interpreter -> the prefix's own fused glibc loader, maintainer-script shebangs ->
   `dn-shell`, per-package fixes from `custom/`.

3. **Unpack, then configure, the whole base in one dpkg call.** `dpkg
   --unpack` on every translated base `.deb` (libraries first, tools
   next, strict Pre-Depends order), then one `dpkg --configure -a` —
   every file of the base is already on disk before any `postinst` runs,
   since Debian never declares its own Essential tools as dependencies.
   The base set is then held. `dn-fix-alternatives.sh` and
   `normalize-symlinks.sh` run once over the result.

4. **Write the prefix's permanent apt config and wire the hooks.** From
   here on, the prefix's own `apt.conf` has `DPkg::Pre-Install-Pkgs` ->
   `dn-hook-pre.sh` and `DPkg::Post-Invoke` -> `dn-hook-post.sh`, so every
   later install — through `apt-install.sh`, a user's own `apt install`,
   or a direct `dpkg -i` via the routing wrapper — goes through them
   automatically; see "The apt/dpkg hooks" below.

5. **Finalize and activate.** `make-launchers.sh` (wrappers; also tags
   NSS and direct-syscall binaries), `make-apt-wrappers.sh` (`termux-apt`/
   `termux-dpkg`/`termux-dn-doctor`/`dn-adopt`), and `dn-activate.sh`
   (the `~/.bashrc` block, PATH).

6. **Now `apt install` works, for anything after this point.** A
   subsequent `apt-install.sh PREFIX pkg` is a plain `apt-get install -y`
   — all the translation logic lives in the hooks wired in step 4, not in
   `apt-install.sh` itself.

## Three clarifications

- **Not a manual patch.** Patching is automatic via the apt hooks from
  step 4 onward; the bootstrap's own base install (steps 2–3) runs the
  same translation directly, before those hooks exist to run it.
- **Not an overlay.** There is **no overlay / mount namespace** — user
  namespaces are off kernel-wide (`TODO.md`, "Blocked"). It is a
  **Debian-layout directory prefix** made transparent by three mechanisms:
  patched maintainer scripts, the `LD_PRELOAD` shim, and the syscall tracer.
- **"Native" ≠ custom libc.** It means reusing **Termux's glibc side-install**
  (glibc + `*-glibc` packages) for the `libc6` stand-in, not shipping a
  second one. Nothing is prebuilt-and-shipped either: the shim and
  `dn-trace` are built from source on-device; Termux's `proot` is not used.
  (`native-seed.sh`'s own stub-database approach is the pre-0.2.0 classic
  design's version of this idea — see
  [`classic-design.md`](classic-design.md) and
  [`native-reuse.md`](native-reuse.md) — superseded here by the real
  `libc6` stand-in itself satisfying the dependency.)

One-liner: **runtime first → stand-ins + base, downloaded and translated
as a batch → unpack/configure the base once, held → write the permanent
apt config and wire the hooks → launchers/apt/PATH → any `apt install`
works from here on.**

## The apt/dpkg hooks

`setup-apt-prefix.sh` wires two hooks into the prefix's `apt.conf`
(`DPkg::Pre-Install-Pkgs` / `DPkg::Post-Invoke`); the generated `dpkg`
wrapper calls the same two scripts for a direct `dpkg -i`:

- **`dn-hook-pre.sh`** — translates every incoming `.deb`
  (`dn-translate-deb.sh`) before dpkg unpacks it, and refuses one that
  would overwrite a file no package owns (deb-native's own runtime or
  launchers).
- **`dn-hook-post.sh`** — `dn-fix-alternatives.sh` -> `normalize-symlinks.sh`
  -> `make-launchers.sh`. Never fails the transaction.

`normalize-symlinks.sh` runs after **every** install this way, not just
`install.sh`'s own bootstrap: the bind-only tracer needs a normalized
tree, and without it an absolute symlink from a package installed outside
`install.sh` would resolve against the real host root instead of the
prefix.

## End-to-end test

A full E2E is: bootstrap a **fresh** prefix, install a package through
`install.sh`, run it by name through its generated launcher.

```
sh install.sh ~/dn-e2e figlet          # fresh bootstrap + install
~/dn-e2e/usr/lib/deb-native/bin/figlet hi
```

Plus the boundary suites (`tests/shim-libc`, `tests/tracer-nss`) against that
prefix.

Verified on `fe2` (2026-09-26): a fresh prefix `~/dn-e2e` plus `figlet` —
`install.sh` exits `0`, `dn-trace` (fork-lite) is installed, `figlet` runs by
name through its launcher, and `tests/tracer-nss` passes against the fresh
prefix. The mechanism-and-routing boundary is closed, so a fresh E2E is the
full check from empty prefix to a working program.

That run also caught a bug worth keeping in mind: the `apt.conf` heredoc is
**unquoted** (it must expand `$NEWPREFIX`), so the comment's backticks around
`apt-get install` were command-substituted and apt's own output was spliced
into `apt.conf`, making `apt-get` fail with *"Extra junk after value"*. Fixed
by dropping the backticks — any checkout before that fix breaks on a fresh
bootstrap (an existing prefix does not, which is why it hid).
