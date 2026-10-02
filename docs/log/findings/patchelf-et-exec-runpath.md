# Findings: patchelf corrupting an `ET_EXEC` binary's program headers, and moving RUNPATH to the loader (2026-09-30)

> Template: [`templates/docs.template.md`](../../../templates/docs.template.md)
> (fix the relative path to match this file's depth). Read a doc's
> summary and table of contents below before its sections, and read
> its directory's own `README.md` first to confirm this is the right
> doc to open. Create a new doc, instead of extending an existing
> one, when the content is a distinct kind of writing — a new spec
> topic, a new one-off investigation, or a new guide — not just a
> long addition to what a doc already covers.

**Impact: Repo change.** Root-caused via clean-room comparison, fixed
by moving `RUNPATH` out of the static patch step entirely into
`ld-dn`'s per-launch environment; verified. Nothing left open in this
entry.

**Symptom:** `cc1` (gcc-14's compiler proper, installed through this
project's own `apt-get` while chasing the 0.5.0 own-glibc build blocker,
`TODO.md`'s "0.5.0 roadmap") segfaulted with no syscall in flight,
inside glibc's own loader startup, right after it re-resolved `cc1`'s own
path. Original suspicion (recorded in `TODO.md` at the time) was that this
was specific to `ET_EXEC` (non-PIE) binaries at the runtime-loader level —
a `ld-dn`/`native/ld-dn.c` bug. It was not.

## Contents

- [Method: clean-room comparison, not just reading the failing binary](#method-clean-room-comparison-not-just-reading-the-failing-binary)
- [Cross-checked this doesn't contaminate a separate, earlier conclusion](#cross-checked-this-doesnt-contaminate-a-separate-earlier-conclusion)
- [Fix: move RUNPATH out of the static patch step entirely](#fix-move-runpath-out-of-the-static-patch-step-entirely-into-ld-dns-per-launch-environment)
- [General lesson for this codebase](#general-lesson-for-this-codebase)

## Related docs

- [`../../spec/android-platform.md`](../../spec/android-platform.md) —
  the sandbox-limits probe this entry cross-checks against
  (`binfmt_misc` not mounted).
- [`gcc-hello-pt-interp-gap.md`](gcc-hello-pt-interp-gap.md) — a later
  entry hitting the complementary `PT_INTERP`-is-a-kernel-level
  problem, the one thing this fix's approach (move it into `ld-dn`)
  cannot reach.

## Method: clean-room comparison, not just reading the failing binary

The project's own apt cache directory is not a safe "before" sample —
`dn-translate-deb.sh` translates each `.deb` **in place**
(`mv -f "$WORK/out.deb" "$DEB"`, overwriting the cached file itself), so a
copy pulled from `var/cache/apt/archives/` is already post-translation.
Downloaded the identical version straight from `deb.debian.org`
(`cpp-14-aarch64-linux-gnu_14.2.0-19_arm64.deb`) for a genuinely pristine
`cc1`, then replayed the translation pipeline's two `patchelf` calls
**one at a time**, `readelf -l`-ing after each, instead of only inspecting
the final (already-broken) result:

| step | program headers | header-`LOAD` `MemSiz` | overlap with the code `LOAD`? |
|---|---|---|---|
| pristine (upstream Debian) | 11 | — (headers/code share one `R E` segment) | — |
| `patchelf --set-interpreter` only | 13 | `0x380` (sane) | no |
| `--set-interpreter` **+** `--set-rpath` | 13 | `0x292d88` (~2.7 MB — a "headers" segment should be a few KB) | **yes** |

Isolated to `--set-rpath`: `cc1` ships with **no RPATH at all**
(`patchelf --print-rpath` on the pristine binary returns empty), so
`dn-translate-deb.sh`'s rewrite has to insert a `DT_RPATH`/`DT_RUNPATH`
entry from scratch — on an `ET_EXEC` binary with no layout slack (unlike
PIE, which always has ASLR-driven headroom), patchelf 0.19.1 miscomputed
the new segment boundary and left two `PT_LOAD`s overlapping: one `RW`
(the header segment, now wrongly sized) and one `R E` (the code). The
kernel maps both at `execve()` time, before any of this project's own code
runs; the second (`R E`) mapping, `MAP_FIXED`, clobbers whatever the first
held in the overlap — in `cc1`'s case, its own `PT_DYNAMIC`, which glibc's
loader then reads while building `cc1`'s `link_map` and dies on.

## Cross-checked this doesn't contaminate a separate, earlier conclusion

that used the same pipeline: `docs/android-seccomp-audit.md`'s "stock
Debian `libc6` doesn't even reach a syscall question" test (2026-09-30)
also ran `dn-translate-deb.sh` and also hit a same-shaped
crash-with-no-syscall-in-flight. Re-translated a pristine
`libc6_2.41-12+deb13u4_arm64.deb` (`deb.debian.org`) through the real,
unmodified-at-the-time pipeline: `libc.so.6`, `ld-linux-aarch64.so.1`
(both `ET_DYN`) and the `hello` test binary (also `ET_DYN`/PIE) all came
out with clean, non-overlapping segments. The bug is `ET_EXEC`-only; the
seccomp-audit conclusion (own-glibc's Android patch series is
load-bearing at loader-startup level) stands independent of it.

## Fix: move RUNPATH out of the static patch step entirely, into `ld-dn`'s per-launch environment

`dn-translate-deb.sh`'s own comment already
named the reason a per-file `RUNPATH` rewrite was needed in the first
place — `RUNPATH` is not inherited transitively (a library's own
`RUNPATH` does not help *its* dependencies' search), so every `.so` in a
package needed the same rewrite, not just the top-level executable.
`LD_LIBRARY_PATH`, set once by `ld-dn` (`native/ld-dn.c`, which already
builds a custom environment for every program it launches — `LD_PRELOAD`,
`DN_INSTDIR`) is consulted for every library load in the whole process,
transitively, for free. Checked for a kernel-native shortcut first
(`binfmt_misc`, which would let the kernel pick an interpreter without any
per-file `PT_INTERP` patch at all): not mounted in the app sandbox
(`/proc/sys/fs/binfmt_misc/register`: "No such file or directory"),
consistent with the namespace/mount limits already probed
(`../../spec/android-platform.md`, "Device probe: sandbox limits confirmed
directly") — no shortcut exists, `--set-interpreter`
stays a required static patch (and is not itself buggy: tested alone
above, clean on `ET_EXEC`).

Landed: `native/ld-dn.c` now also sets `LD_LIBRARY_PATH` (prefix lib dirs)
and `COMPILER_PATH` (found separately, same session: gcc's own subprogram
search — `cc1`, `as`, `ld` — does not fall back to a plain `$PATH` walk
the way a shell does, only its own compiled-in target-triplet directories
plus `COMPILER_PATH`; without it gcc silently ran Termux's own `ld.lld`
instead of the prefix's binutils). `dn-translate-deb.sh`'s ELF loop drops
to `--set-interpreter` only; `dn-adopt.sh` drops its missing-library
detection and `--set-rpath` call for the same reason. Verified: `cc1 -v`
runs (was segfaulting), `gcc-14 -S` on a headers-free test file produces
correct AArch64 assembly, and `gcc-14 -nostdlib -static` now links against
the *prefix's* `ld` without `COMPILER_PATH` set by hand.

## General lesson for this codebase

When a static per-`.deb` ELF patch
step (`dn-translate-deb.sh`, `dn-adopt.sh`) and `ld-dn`'s per-launch
environment could both solve the same problem, prefer the loader — it
already runs once per program with full env control, one code path
instead of N patched files, and no risk of a third-party tool (`patchelf`)
miscomputing a static binary's layout. `--set-interpreter` is the one
thing that cannot move (the kernel reads `PT_INTERP` at `execve()`,
confirmed no `binfmt_misc` escape hatch on this device) — everything else
patched today is worth re-checking against this question before assuming
it has to stay static.
