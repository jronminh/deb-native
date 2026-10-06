# Package lifecycle — one `.deb`, from index to running

<!-- template: templates/docs.template.md -->

The lifecycle of a **single package** in a prefix: `apt update` through to the
program running by name. It maps each stage to the dpkg/apt hook that fires,
the script that does the work, and the layer that makes it stick -- so a review
can pick one stage and read only what belongs to it. `design.md`'s "Install
pipeline" is the design record behind the two translation hooks.

Living doc: it tracks the shipped pipeline, not a proposal.

## Contents

- [Stage map](#stage-map)
- [Stages in detail](#stages-in-detail)
- [Removal and upgrade](#removal-and-upgrade)
- [Timing traps](#timing-traps)

## Related docs

- [`design.md`](design.md) — the live design; "Install pipeline" is this
  doc's two hook rows in context.
- [`path-shim.md`](shim/path-shim.md) — the shim and maintainer-script layers.
- [`tracer.md`](tracer/tracer.md) — `dn-trace`, the syscall layer at run time.
- [`syscall-boundary.md`](../reference/syscall-boundary.md) — what the shim cannot see.
- `scripts/prefix/` — the scripts per lifecycle category.

## Stage map

| # | Stage (apt/dpkg) | What fires | Layer |
|---|---|---|---|
| 0 | `apt update` — index | `core/install/dn-debian-index.sh`: rewrite `Architecture: all` -> `arm64` | prefix's own apt |
| 1 | resolve + download | real `apt`, run through the `$DN/bin/apt*` launchers | prefix's own apt/dpkg |
| 2 | `DPkg::Pre-Install-Pkgs` | `dn-hook-pre.sh` -> `dn-translate-deb.sh` (+ `patch-scripts-tree.sh`, `core/custom/`), then a collision check | `.deb` translation |
| 3 | `dpkg --unpack` — `preinst` | `preinst` through the prefix's `dn-shell` | maintainer script + shim |
| 4 | `dpkg --configure` — `postinst` | `postinst`/`configure` + dpkg helpers/triggers | maintainer script + shim |
| 5 | `DPkg::Post-Invoke` | `dn-hook-post.sh` -> `dn-fix-alternatives.sh` -> `normalize-symlinks.sh` -> `make-launchers.sh` -> `dn-fix-gcc-specs.sh` | prefix fixups |
| 6 | run by name | launcher -> `dn-run` classifies, then the prefix loader/shim/`dn-trace` | runtime |

The two hooks (stages 2 and 5) are the whole per-install pipeline; everything
else is dpkg's own order, left intact on purpose (`design.md`).

## Stages in detail

**0. Index.** `dn-debian-index.sh` makes the Debian index look native, so dpkg
accepts packages without `--force-architecture`; `arm64` stays a foreign
architecture rather than being relabelled `aarch64` (`design.md`). Bootstrap
runs this before the base batch; a later `apt update` by the user goes through
the same script.

**1. Resolve and download.** The prefix uses Termux's real `apt`/`dpkg` through
launchers that pass the prefix explicitly (`APT_CONFIG`, `--admindir`,
`--instdir`) -- no rebuilt package manager, so Termux's updates carry through.

**2. Translate before dpkg sees it (`DPkg::Pre-Install-Pkgs`).**
`dn-hook-pre.sh` reads apt's hook-protocol plan (version 3) on stdin, or `.deb`
arguments for a direct `dpkg -i`, and for every `.deb` about to be unpacked:
`dn-translate-deb.sh` relabels the control file, repoints ELFs at the `libc6`
stand-in, rewrites maintainer-script shebangs to the prefix's shell
(`patch-scripts-tree.sh`) and applies `core/custom/<package>.sh` fixes -- in one
unpack/repack, several packages in parallel (`DN_JOBS`). A collision check then
refuses a `.deb` that would overwrite a file no package owns (deb-native's own
runtime/launchers). **A failure here fails the hook, so apt runs nothing.**
Because translation happens here, `apt-install.sh` itself is a plain
`apt-get install -y`.

**3. Unpack, `preinst`.** dpkg unpacks the translated files into the prefix;
`preinst` runs as a maintainer script inside dpkg's `--unpack`, i.e. before
`--configure`, and outside the process deb-native controls -- the reason the
maintainer-script exec path exists (`core/native/dn-launch.c`).

**4. Configure, `postinst`.** dpkg runs `postinst`/`configure` and triggers.
dpkg's helpers are wrapped so they compute prefix paths themselves
(`update-alternatives` with `--altdir`/`--admindir`, `dpkg-divert`,
`dpkg-statoverride` as a no-op, `dpkg-trigger`) -- each root-caused with
`strace` (`design.md`).

**5. Fix up after the transaction (`DPkg::Post-Invoke`).** `dn-hook-post.sh`
runs, in order: `dn-fix-alternatives.sh` (make `update-alternatives` links
relative), `normalize-symlinks.sh` (every absolute symlink inside the prefix
relative, so the kernel -- and the bind-only tracer -- never resolves outside
it), `make-launchers.sh` (one launcher per program, also tagging NSS and
direct-syscall binaries), and `dn-fix-gcc-specs.sh` (point an installed gcc's
default dynamic linker at the prefix's own fused glibc loader). Unlike the pre-hook, **this hook never
fails the transaction.**

**6. Run by name.** The launcher (`make-launchers.sh`) puts `dn-run` in front
of the program; `dn-run` classifies the target ELF and picks the shim, a plain
exec, or the tracer (`dn-trace`). Every translated ELF names the prefix's own
fused glibc loader as its interpreter, which reads the shim from
`ld.so.preload` and hands over to glibc -- runtime detail in `path-shim.md`,
`tracer.md`, `syscall-boundary.md`.

## Removal and upgrade

`dn-hook-pre.sh`'s protocol also carries `REMOVE`/`**CONFIGURE**` actions; only
`.deb`s are translated, so a removal has nothing to translate. `prerm`/`postrm`
run through the same maintainer-script path as stages 3-4. Full behaviour
across an upgrade and a clean `install.sh --uninstall` is still open
(`TODO.md`, "Upgrade and remove"), so this doc describes install, not removal,
as built.

## Timing traps

- **`preinst` precedes `postinst` by design.** In the base set assembled at
  build time every file is already on disk before any `postinst` runs, because
  Debian never declares its own Essential tools as dependencies. A later
  single `apt install` follows dpkg's normal Pre-Depends order instead.
- **Pre-hook failure is transactional, post-hook failure is not.** Stage 2
  exiting non-zero stops apt before it unpacks anything; stage 5 is
  best-effort by contract.
- **The post-hook runs after every install this way.** `normalize-symlinks.sh`
  in particular must, or an absolute symlink from a package resolves against
  the real host root.
- **Maintainer scripts run under the prefix's own shell**, never Termux's --
  a shebang that survives translation points at `dn-shell`.
