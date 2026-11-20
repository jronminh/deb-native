# Provenance

This working tree consolidates snapshots and selected findings. The source
commit IDs identify the material reviewed.

| Source repo and ref | Source commit SHA | Material taken | Destination |
| --- | --- | --- | --- |
| `deb-native` `main` | `1f7f74f435658f9401795c5e57cad30c2500e160` | Public implementation tree, docs, license, project metadata and issue templates. Reframed docs around FOA and removed local setup details. | Root, `src/`, `scripts/`, `tests/`, `docs/`, `.github/` |
| Private research notes archive | | Edited findings from `android-seccomp-syscall-map.md`, `android-vs-linux-foreign-libc.md`, `android-userspace-from-generic-linux.md`, `running-deb-in-a-prefix.md` and `running-outside-the-app-domain.md`. | [notes/](notes/README.md) |
| Private apt-feed scaffold | | Historical translated apt feed proposal, license, ignore rules, seed lists and build/customization placeholders. | [apt-feed/](apt-feed/README.md) |
| Retired dn-shell archive | | Terminal APK and later bionic host-tree design described in the retirement note. | [archive/dn-shell/](archive/dn-shell/README.md) |
| Archived Debi No Root proof of concept | | TTY behavior, headless test results, and S10e APK measurement and packaging. | [archive/debi-no-root.md](archive/debi-no-root.md) |
| `fhs-on-android` | [FOA pin](refs/foa.md) | Method levels and page paths verified against the FOA worktree. | [refs/foa.md](refs/foa.md) |

The consolidation design informed the selection and destination of the
material. Each included finding was checked against its source snapshot.
