# deb-native vs sudo-less: the diff

Both projects install stock Debian `.deb`s into a private prefix as an
unprivileged user and make the installed programs work. The difference is
**the base system and the virtualization mechanism**:

- **sudo-less** is Debian-on-Debian: a non-root account on a real Debian
  host, its own fork of apt/dpkg, and a kernel **"view"** — a private
  mount namespace with an unprivileged overlayfs that overlays the prefix
  onto `/usr /etc /var /opt`.
- **deb-native** is Termux fused with a userspace **native overlay**: an
  unrooted Android app's Bionic Termux as the host, Termux's own apt/dpkg
  and glibc side-install (`termux-pacman/glibc-packages`) as the base, and
  our own path-redirect shim plus a Bionic ELF launcher as the
  "overlay" — no namespaces at all, because this device's kernel does not
  allow them.

Same goal, same "never root" rule; the whole mechanism is inverted because
the host and the kernel are different.

## The diff

| concern | sudo-less (Debian host) | deb-native (Termux/Android) | delta |
|---|---|---|---|
| host | standard Debian, non-root user, `~/.local` | unrooted Android app (uid `u0_a…`), Termux prefix `/data/data/com.termux/files/usr` | different base, same no-root constraint |
| apt/dpkg | fork of Termux's patches, retargeted *back* to Debian | Termux's own apt/dpkg, built for Bionic, reused **as-is** | the reuse runs in the opposite direction |
| files land in | `~/.local` | a dedicated `$INSTDIR` (e.g. `~/.dn/root`), separate from Termux's own prefix | same idea, different target |
| path virtualization | kernel **view**: `unshare(CLONE_NEWUSER)` + unprivileged overlayfs over `/usr /etc /var /opt` | userspace: our own `path-redirect.so` shim rewrites `/usr /etc /var /opt` → `$INSTDIR`, with `execve` dispatch and shebang handling | view is **dead here** (`unshare` = `EINVAL`, FUSE closed); the shim replaces *path resolution only* |
| glibc / libc | host already has glibc; prefix seeded from the host's dpkg db | Termux has **no** glibc; reuse Termux's `$PREFIX/glibc` side-install for libs, coreutils, perl; `native-seed.sh` stubs Debian names | the second userland is the fusion's whole point |
| maintainer-script runtime | scripts run inside the view; the kernel's `/bin/sh` sees the prefix | `native/dn-launch.c`, a Bionic ELF, execs Termux's glibc `bash` (`dn-shell`) / `perl` (`dn-perl`) with the shim; control-script shebangs are rewritten to it | a real ELF is required — the kernel follows only one `#!`, and Android's `/system/bin/sh` is root-owned toybox |
| interpreter / ELF | host `/lib/ld-linux-…` already correct | `grun --configure` repoints Debian ELFs at `$PREFIX/glibc/lib/ld-linux-aarch64.so.1` | glibc-runner is fused in |
| root-only helpers | shims on `DPkg::Path` only (`py3compile`, service helpers) | shims on the runtime `PATH` (`chown`, `chgrp` no-op); a missing Termux `dpkg-realpath` data file is pre-placed | same class, different helpers |
| two-layer database | seed the prefix from the **host's** dpkg status so host libraries count as installed | seed from Termux's installed `*-glibc` packages so *those* count; only the delta lands in `$INSTDIR` | seeding runs in opposite directions |
| architecture | host arch, native | Debian `arm64` on Android `aarch64` — archive name mismatch, worked around with `--force-architecture` | still-open, flagged unsafe |
| isolation at install | install view hides `$HOME`, empty `/run`, so a hostile package can't reach your files or poke host services | **none** — maintainer scripts run with normal access to the user's home | unbuildable here (no namespaces) |
| run-time wrappers | `prefix-wrap`: per-binary wrappers from detection heuristics (interpreter, module paths, `ldd`, `/usr` paths, i386 loader) | **not built** (design only: `design.md`); the shim covers hardcoded paths when a program is launched through `dn-shell` | the biggest unbuilt run-time piece |
| services | `prefix-units`: systemd unit → `systemd --user` unit, sandboxed; `prefix-sandbox` seccomp | **not built**; Termux has no systemd, candidate is `termux-services`/runit (research only) | view-dependent and unbuilt |
| classifier / refusal | stage 2 `prefix-check`: scope + mechanism + unsafe scripts, refuses before dpkg | **not built** | — |
| integration | launchers, icons, desktop DB, alternatives | **not built** (largely N/A on Android) | — |
| state / `explain` / `doctor` | none yet either | none | comparable |
| coverage | **63%** real install (127 pkgs); 73% predicted need nothing, 17% overlay | **33%** (30-pkg sample) *before* the base-env fix; **unmeasured** after | needs a fresh survey |

## What the fusion buys, and what it costs

**Buys:** a full glibc userland (bash, coreutils, perl, binutils …) that
already exists before the first Debian package is installed — which is
what removes sudo-less's "need a working shell first" cycle — and a
`/usr/glibc` that the Debian ELFs can be repointed at with `grun`, so a
real glibc `.deb` runs unmodified.

**Costs:** no kernel view means no isolation rows (hidden `$HOME`, empty
`/run`, service view, other-architecture loaders), and the shim only
substitutes for the view's *path-resolution* job. Everything the view did
beyond that has to be rebuilt differently (run wrappers, runit services)
or is simply out of reach on this device.

## What is left, in one line

Install core (apt/dpkg + prefix + base bootstrap + glibc fusion) is in
place; the run/integrate/classify/serve half is mostly unbuilt, and the
isolation/service-view rows are architecturally unavailable here. Roughly
**40–45% of sudo-less's overall functionality**, with the next
highest-leverage step being a fresh coverage survey.
