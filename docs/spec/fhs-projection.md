# The FHS projection: where each Debian path lands on Android

<!-- template: templates/docs.template.md -->

A reference table, and only that: for each typical directory of Debian's
filesystem tree, the real Android path it resolves to and the runtime part that
stands in the way of the access. It states what the code does today, not what it
should do; the proposals that build on it live elsewhere. Living doc, written
from `src/dn-policy/dn-policy.c`, `src/tracer/cli/dn-trace.c` and a core-deb
`0.7.1-dev` prefix measured on a Samsung SM-G970W (Android 12), 2026-10-13.

## Contents

- [Notation](#notation)
- [The law in one paragraph](#the-law-in-one-paragraph)
- [The table](#the-table)
- [The table's own state](#the-tables-own-state)
- [What applies to every row](#what-applies-to-every-row)
- [Open items](#open-items)

## Related docs

- [`runtime.md`](runtime.md) — the runtime and dn-policy's "Path rewriting".
- [`monitor.md`](monitor.md) — who catches, who decides, who executes.
- [`emulation.md`](emulation.md) — the operations the platform refuses; `dn-emu`.
- [`prefix.md`](prefix.md) — the tarball layout this table is read from.
- [`native.md`](native.md) — the front-end a Debian binary sees.

## Notation

- `T` is the prefix root on the device:
  `/data/user/0/<package>/files/prefixes/<NAME>` (the app's own files directory;
  the runtime derives it from `dn-monitor`'s location, `prefix.md`).
- Tools, as in [`monitor.md`](monitor.md):
  - **dn-shim → dn-trace**: the mapping law applied in-process by the shim first;
    what the shim cannot see (static and Go programs, raw `svc #0`, libc-internal
    calls) falls to dn-trace over `ptrace`.
  - **dn-emu**: the second authority for refused or divergent operations. It does
    not exist yet; where a row names it, the work today is done by pieces that
    sit in the tracer (`emulation.md`, "Already emulated").
  - **script**: a shell script that runs inside the guest (`boot/init.sh`,
    `boot/bootstrap.sh`), not a runtime part.
- Markers: **✓** confirmed by the code or by a measurement on a device; **~**
  inferred or read from a doc, not confirmed; **✗** not implemented, or a gap.

## The law in one paragraph

A guest path is rewritten into a host path; the program never sees the host
path. `/` maps to `T`, and the longest matching prefix wins. A path already under
`T` is returned unchanged (translation is idempotent, "no double translation"),
and `T` is also bound to itself so a host path inside the tree stays valid.
`/proc`, `/sys` and `/dev` are the only rows that map to themselves (the host's
own files). Absolute symlinks inside the tree are resolved component by
component against `T`; relative symlinks are left to the kernel. A relative
guest path is left to the kernel as well, resolved against the real working
directory. `getcwd` and the `/proc/self/{cwd,exe,fd/N}` links are translated
back to guest paths.

## The table

| 1. Debian FHS directory | 2. Android target | 3. Tool |
| --- | --- | --- |
| `/` | `T` | dn-shim → dn-trace ✓ |
| `/bin`, `/sbin`, `/lib` | relative links to `usr/bin`, `usr/sbin`, `usr/lib` inside `T` (merged `/usr`) | the kernel resolves the link; the path still goes through dn-shim → dn-trace ✓ |
| `/usr/{bin,sbin,lib,libexec,share,include,src,games}` | `T/usr/...` | dn-shim → dn-trace ✓ |
| `/usr/lib/deb-native` (`RT`) | `T/usr/lib/deb-native`: `dn-monitor`, `dn-interp`, `dn-shim.so`, `repo/` | outside dpkg; `dn-monitor` is the root process, `dn-interp` delivers the shim ✓ |
| `/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1` | `T/usr/lib/aarch64-linux-gnu/...` | the exec gate of dn-trace routes a glibc-dynamic program through it; `PT_INTERP` is never rewritten ✓ |
| `/etc` | `T/etc` | dn-shim → dn-trace ✓ |
| `/etc/passwd`, `/etc/group` | `T/etc/...`, written on the first boot | script (`base-passwd` preinst, run by `bootstrap.sh`) ✓ |
| `/etc/resolv.conf` | a relative link `../../app/etc/resolv.conf` that does not resolve; `init.sh` then writes `nameserver 8.8.8.8` | script (`boot/init.sh`) ✓; the device's DNS is not used ✗ |
| `/etc/ld.so.cache` | none (the artifact carries no cache) | not applicable ✓ |
| `/var` | `T/var` | dn-shim → dn-trace ✓ |
| `/var/lib/dpkg` | `T/var/lib/dpkg` | dn-shim → dn-trace, plus the owner store (`user.dn.*` xattr, fake root) ✓ |
| `/var/run`, `/var/lock`, `/var/tmp` | `T/var/...`, real directories (`/var/run` is not a link to `/run`) | dn-shim → dn-trace ✓ |
| `/run` | `T/run`, an empty real directory (not a tmpfs) | dn-shim → dn-trace ✓ |
| `/tmp` | `T/tmp` | dn-shim → dn-trace ✓ |
| `/home`, `/root` | `T/home`; `/root` is a link to `home` | dn-shim → dn-trace ✓; `HOME=/root` is set by the script `boot/init.sh` |
| `/boot` | `T/boot`: `boot.sh`, `init.sh`, `bootstrap.sh`, `install.sh`, `version` (the host's interface, not a kernel) | dn-shim → dn-trace ✓ |
| `/mnt` | a relative link `../mnt`, that is `prefixes/mnt`, which does not exist | none ✗ |
| `/media`, `/opt`, `/srv` | `T/...`; absent from the tarball, present after the first boot (probably created by `base-files`) | dn-shim → dn-trace ~ |
| `/dev` | `/dev`, the host's own | a direct row of dn-policy: passed through by both executors ✓; `/dev/full` is missing (`TODO.md`) |
| `/dev/shm` | the tracer has a dedicated mapping (`src/tracer/tracee/seccomp.c`); the exact target is not confirmed | dn-trace, plus a faked `statfs` reporting tmpfs (the part of `dn-emu` that exists today) ~ |
| `/proc` | `/proc`, the host's own | a direct row ✓; `/proc/self/root`, `/proc/self/cwd/..` and `/proc/self/fd` reach the host's real root ✗ |
| `/proc/self/{cwd,exe,fd/N}` | the host's, reverse-translated to guest paths | `dn_policy_detranslate_path`, in both executors ✓ |
| `/sys` | `/sys`, the host's own | a direct row ✓ |
| `/data` (not in the FHS) | `T/data/...`, a skeleton of empty directories | a side effect of binding `T` to itself ~ |

## The table's own state

- The three direct rows (`/proc`, `/sys`, `/dev`) are written into
  `load_bindings()` in `src/dn-policy/dn-policy.c`.
- `RT/state/bindings` (`host<TAB>guest` per line, at most 32 rows in all) is read
  by `dn_policy_init()` and reaches both executors. The artifact ships none.
- `dn-monitor -b HOST[:GUEST]` adds a row to the tracer's table. From the code it
  does not reach dn-policy's table, so the shim would not see it ~ (not tried on
  a device).
- A more specific guest row wins over a less specific one (`/dev/shm` over
  `/dev`).

## What applies to every row

- **Identity**: `uid`/`gid` are faked to 0; `chown` and `chmod`'s setuid bits go
  to the owner store; `stat` reports from it.
- **Hardlinks**: `link()` is refused by SELinux in app data, so a hardlink
  becomes two symlinks to a hidden file, and `stat` reports a regular file with
  the right count.
- **`mknod`** of a device makes an empty regular file and records the type.
- **`mount`, `umount`, `pivot_root`, `unshare`, `setns`** are recorded in a
  binding table and answered with success; nothing is mounted.

## Open items

- `/etc/resolv.conf` and `/mnt` are links that assume a sibling `app/` and
  `mnt/` of the prefix, which the app's tree layout does not provide.
- `/proc` maps to the host's own; the unfaked `/proc/self/root` and its
  neighbours are reachable from inside the tree (`emulation.md`, "Synthetic /proc
  and /sys", is the planned answer).
- Whether `-b` rows reach the shim.
- `native.md` ("The front-end") says `/tmp` is the host's; the code maps it into
  `T`.
- The target of `/dev/shm`, and whether `/media`, `/opt`, `/srv` belong in the
  artifact rather than appearing at the first boot.
