# Install flow — what happens, in order

From nothing to "any `apt-get install` works". Grounded in the scripts; the
mechanisms each step installs are documented in
[`design.md`](design.md) (shim/tracer/maintainer-script layers),
[`syscall-boundary.md`](syscall-boundary.md) (what each layer reaches), and
[`bind-only.md`](bind-only.md) (the tracer's path fast path).

## The sequence

1. **Runtime first, before dpkg touches anything.** `apt-hook-pre.sh:27` runs
   `setup-runtime.sh` on every install (also from `patch-deb.sh:27` and
   `patch-maintainer-scripts.sh:42`). It builds/installs:
   - the **shim** `path-redirect.so` (`setup-runtime.sh:37`, glibc layer);
   - **`dn-run`** (`:45`), the launch classifier, and **`dn-shell`/`dn-perl`**;
   - the **tracer** as `dn-trace` (fork-lite) if `tracer/proot` is present,
     else `dn-run` falls back to Termux `proot`.
   Ordering is load-bearing: maintainer scripts run during dpkg's `--unpack`,
   so the interpreter and shim must already exist.

2. **Patch each `.deb` inside the archive, before unpack.** `apt-hook-pre.sh`
   → `patch-deb.sh` rewrites control scripts (shebang → the `dn-shell`
   wrapper, hardcoded paths) because a package's `preinst` runs during
   `--unpack`, before any post-unpack step could see it. After unpack,
   `patch-elfs.sh` (`apt-hook-post.sh:15`, and per package in
   `apt-install.sh:60`) repoints each new ELF's interpreter at Termux glibc,
   and `patch-maintainer-scripts.sh` re-sweeps once the wrapper exists.

3. **Seed the native side.** `native-seed.sh` (`setup-apt-prefix.sh:71`)
   writes stubs into the prefix's dpkg status so Debian's `Depends: libc6,
   libssl3, …` are satisfied by Termux's `*-glibc` packages instead of being
   reinstalled. This is the "native" in deb-native: one libc, reused.

4. **Bootstrap the base as one transaction.** `bootstrap-base.sh` installs
   `mawk base-files base-passwd dash debianutils debconf cdebconf openssl
   ca-certificates` via `apt-install.sh`'s three phases:
   download-only (apt resolves the graph) → patch each `.deb` →
   unpack / `patch-elfs` / re-patch scripts / configure **one package at a
   time in apt's order** (strict Pre-Depends like base-files→awk need this).

5. **Finalize and activate.** `make-launchers.sh` (wrappers; now also tags
   NSS and direct-syscall binaries), `make-apt-wrappers.sh` (arch-aware
   apt/dpkg), `dn-activate.sh` (PATH). `install.sh` then runs
   `normalize-symlinks.sh` for the bind-only tracer.

6. **Now `apt-get install` works.** The apt.conf hooks
   (`setup-apt-prefix.sh:67-68`, `DPkg::Pre-Install-Pkgs` / `Post-Invoke`) keep
   steps 1–2 and launcher regeneration wired for every later install.

## Three clarifications

- **Not a manual patch.** Patching is automatic via the apt hooks;
  `apt-install.sh` also patches explicitly as a safety net. "Base" is just the
  one-transaction bootstrap set (`bootstrap-base.sh`).
- **Not an overlay.** There is **no overlay / mount namespace** — user
  namespaces are off kernel-wide (`TODO.md`, "Blocked"). It is a
  **Debian-layout directory prefix** made transparent by three mechanisms:
  patched maintainer scripts, the `LD_PRELOAD` shim, and the syscall tracer.
- **"Native" ≠ custom libc.** It means reusing **Termux's glibc side-install**
  (glibc + `*-glibc` packages) via `native-seed.sh`, not shipping a second
  one. Nothing is prebuilt-and-shipped either: the shim and `dn-trace` are
  built from source on-device; `proot` is only a fallback.

One-liner: **runtime (shim + `dn-run` + tracer) → auto-patch each `.deb` →
seed native glibc deps → bootstrap base in one transaction → wire
launchers/apt/PATH → any `apt-get install` works.**

## End-to-end test

A full E2E is: bootstrap a **fresh** prefix, install a package through
`install.sh`, run it by name through its generated launcher.

```
sh install.sh ~/dn-e2e figlet          # fresh bootstrap + install
~/dn-e2e/root/usr/lib/deb-native/bin/figlet hi
```

Plus the boundary suites (`tests/shim-libc`, `tests/tracer-nss`) against that
prefix.

Readiness: the mechanism-and-routing boundary is closed (cases 1–5 route to a
working mechanism), and `install.sh` reuse-mode was exercised on `fe2`. What a
fresh E2E would newly cover is the full `setup-apt-prefix.sh` path with the
tracer install and `normalize-symlinks.sh` in it.
