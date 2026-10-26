# Runtime-revision audit

An audit of the 94-commit "runtime revision" (`docs/spec/runtime-revision.md`,
`dn-tiny` -> `dn-init` + `dn-exec`), done by reading `src/dn-exec/` and
`src/dn-init/` against the device and, where practical, confirming with small
programs and `tests/bench/run.sh` on `/data/data/com.termux/files/p-dev`
(gcc-14). One bug was already found and fixed before this audit started
(`14ffb27`, the bss-tail overlay); this is what else turned up.

**How to tell confirmed from theoretical** (used throughout): confirmed means
reproduced with a concrete command and its output; theoretical means read from
the code and the documented kernel/ptrace semantics, with no reproduction
attempted or achieved.

## Confirmed: `O_DIRECTORY`'s wrong value breaks the `DN_ROOT_FD` handoff

`src/dn-exec/dn-exec.c:43` defined `O_DIRECTORY` as `0x10000` (the value most
people remember from the generic/x86 Linux ABI). On this device's actual
aarch64 kernel ABI the real value is `0x4000`; `0x10000` makes the kernel
return `EINVAL`. Confirmed three ways:

- A freestanding raw-syscall probe (no libc) against several ordinary host
  directories: `openat(..., O_RDONLY|0x10000)` -> `-22` (EINVAL);
  `openat(..., O_RDONLY|0x4000)` -> succeeds.
- Bionic/Termux's own `python3`: `os.O_DIRECTORY == 0x4000`.
- The prefix's own glibc, compiled and run *inside* the prefix under the real
  tracer (`gcc`+a one-line program printing `O_DIRECTORY` from `<fcntl.h>`):
  also `0x4000`.

Used at `src/dn-exec/dn-exec.c:485`: `openat(AT_FDCWD, root,
O_RDONLY|O_DIRECTORY, ...)`, whose fd is `dup3`'d to `ROOT_FD` (200,
`dn-exec.c:37,488`) for the shim to read as `DN_ROOT_FD`
(`src/dn-init/lane.c:178-209`, `own_root()`). With the wrong constant the
`openat` always failed, the `dup3` was skipped, yet `dn-exec.c:502` still set
`DN_ROOT_FD=200` in the child's environment unconditionally (gated only on
`root[0]`, not on the open having succeeded). The shim's `own_root()` then
fails the `readlink("/proc/self/fd/200")` and silently falls back to
`dladdr()`/`root_from_maps()` -- the mechanism keeps working through the
fallback, so nothing crashes and no existing test caught it, but the specific
thing `docs/spec/runtime-revision.md` calls "resolved in practice" (handing the
root down as a reserved fd instead of self-discovery) has never actually run on
this device.

Fix: `src/dn-exec/dn-exec.c:43`, `0x10000` -> `0x4000`, and the env entry is
written only after the `dup3` succeeds (next paragraph). After the fix:
`openat`+`dup3` both succeed under the real tracer (instrumented build,
`rfd=3`/`dup3=200`); `tests/bench/run.sh check` passes (the one `shm: FAIL` is
`/run/shm` missing from this particular test prefix, unrelated to the fix --
confirmed present/absent the same with and without it); `tests/bench/run.sh
bench` A/B (several interleaved runs each way) shows no timing or stop-count
regression, and the `reg` digest is byte-identical.

**The handoff is one-shot, which is part of why nothing saw this.** `lane.c`'s
`own_root()` reads `/proc/self/fd/200` and closes the fd (`lane.c:195`), so the
program itself never finds it: `readlink /proc/self/fd/200` under `dn-exec`
answers `ENOENT` *with* the fix too, and correctly. What is observable is the
child's environment. `dn-exec` used to write `DN_ROOT_FD=200` whenever `--root`
was given, whether or not the `openat` had worked -- so the variable proved
nothing. It now writes it only after `openat`+`dup3` both succeed
(`have_root`, `dn-exec.c:484-502`), which makes the entry's presence the proof
that the handoff happened, and the one line a shell test can assert. Verified
on `p-dev`: `dn-exec --root P /usr/bin/env` lists `DN_ROOT_FD=200`, and
`tests/dn-exec/run.sh`'s "root fd handoff" step checks it.

## Theoretical, not reproduced: fork/vfork/clone event-ordering race

`src/dn-init/catch.c:531-537` processes `PTRACE_EVENT_FORK/VFORK/CLONE`
(reported on the *parent*) by calling `trace_exe_fork(parent, child)`
(`src/dn-init/apply.c:2056-2069`), which copies the parent's ids/exe state onto
the child's `pid_state`. Separately, `catch.c:540-548` (the "signal-delivery
stop" branch, which is where the *child's own* initial SIGSTOP is handled)
calls the `EXP(p)` macro (`catch.c:55`:
`pid_state_get((p),1)->expect_exit`), which auto-creates a `pid_state` for an
unknown pid with default/seed ids if one doesn't exist yet
(`apply.c:43-67`, `pid_new()`: `s->ids = seed_ids`).

Linux's ptrace semantics do not guarantee which of these two stops (the
parent's event vs. the new child's own initial stop) a single-threaded
tracer's `wait4(-1, ...)` sees first. If the child's stop is processed first,
`EXP(p)` creates its state with default ids before `trace_exe_fork` has a
chance to copy the parent's real ids onto it. The window this leaves open: if
the child manages to run far enough to hit a seccomp-filtered syscall before
the tracer's *next* `wait4` drains the parent's pending fork event, that one
syscall would be classified under the wrong (default/seed) identity instead
of the parent's.

Not reproduced: this needs either a stress harness that hits the scheduling
window reliably, or direct instrumentation of the order the two events
actually arrive in on this device/kernel (4.14.190). Neither was attempted
yet. See "Next" below.

## Fixed: `tests/dn-exec/run.sh` was stale

`tests/dn-exec/run.sh:46` asserted `dn-ldhook.so` is mapped into a dynamic
program under `dn-exec`. `dn-ldhook.so` was deleted in `45737cd` (loader
patching moved into `dn-exec`'s own birth-time `patch_image`, see
`runtime-revision.md`, "Drop ldhook"), so the script failed at that line
(confirmed by running it) and every step after it never ran -- which is why
nothing caught the `O_DIRECTORY` bug above: the suite never got far enough to
exercise `DN_ROOT_FD`.

Fixed in the same change as the bug: the `dn-ldhook.so` assertion is gone (the
`dn-lane.so` one stays), a **root fd handoff** step asserts `DN_ROOT_FD=200` in
a child's environment, and the `find` step now runs from the tree's root --
from the host's cwd it died on "Failed to restore initial working directory",
a second bug the earlier failure had hidden. The whole suite passes on `p-dev`.

Fixed since: `build-runtime.sh` builds `dn-exec` (through `build-dn-exec.sh`,
into the same `OUT`) and `build-prefix.sh` installs it, refusing a runtime that
lacks it -- a missing `dn-exec` used to mean a silent fallback to the loader
path.  Still stale, left to the documentation pass: `scripts/build-runtime.sh`'s
header comment and `docs/building.md` describe building and shipping
`dn-ldhook.so`.

## Theoretical, not reproduced: `exec_range()` merges disjoint executable segments

`src/dn-exec/dn-exec.c:279-293` (`exec_range`) unions every `PF_X` `PT_LOAD`
segment of a mapped image into one `[lo, hi)` span, which
`patch_image()` (`src/dn-exec/patch.c:136`) then scans uniformly for `svc`
sites (called from `dn-exec.c:444-445` and `468-469`). If an image ever has
two executable segments with a non-executable gap between them, the scanner
would walk that gap as if it were code, risking a false-positive `svc`-site
match in whatever is actually there.

Checked against the only two binaries actually exercised so far --
`/lib/ld-linux-aarch64.so.1` and `/usr/bin/dash` in the `p-dev` prefix -- and
both have exactly one `PF_X` `PT_LOAD` segment each, so this does not
currently misfire. Only a concern for binaries with multiple disjoint
executable segments (hardened/CFI toolchains, or the Go case
`runtime-revision.md` itself already flags as deferred/untested).

## Outcome so far

- **One confirmed bug, fixed**: `O_DIRECTORY` (`dn-exec.c:43`), plus the
  `DN_ROOT_FD` export now gated on the `dup3` succeeding.
- **The `dn-exec` suite fixed and green**: `tests/dn-exec/run.sh` (the
  `dn-ldhook.so` assertion dropped, a root-fd step added, `find` run from the
  tree's root).
- **Two theoretical findings**, neither reproduced: the fork/vfork/clone
  event-ordering race (`catch.c:531-548`), and the disjoint-exec-segment
  scan (`dn-exec.c:279-293`).
- **Checked and found correct, no issue**: `map_image`'s file-backed overlay
  invariant beyond the already-fixed `14ffb27` case; `patch.c`'s
  register-save/restore trampoline; `law.c`'s `under()` boundary check and its
  link-cache seqlock (`lc_get`/`lc_put`); `apply.c`'s owner-store/`dac_fix`/
  `mark_if_new` (the privilege-sensitive DAC-override path).
- **Not yet read**: most of `apply.c` (2873 lines; read so far: the owner
  store and `dac_fix`/`mark_if_new`, roughly 15% of the file) -- in
  particular the AF_UNIX abstract-socket mapping, `/proc` synthesis, and the
  rest of the clone/vfork state-inheritance code. Most of `law.c`'s binds
  parsing and `to_host`/`to_guest` (read so far: the link cache).

## Next

Confirm or rule out the fork/vfork/clone race above: see the proposal in the
audit conversation (a stress harness plus temporary instrumentation of
`catch.c`'s event-ordering, not committed code).
