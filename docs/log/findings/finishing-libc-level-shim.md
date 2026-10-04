# Findings: finishing the libc-level shim, and closing the measured gaps (2026-09-26)

<!-- template: templates/docs.template.md -->

**Impact: Repo change.** `521cc73` closed every item a code review
flagged; what's left belongs to a different layer entirely (the
tracer), already tracked there.

## Contents

- [What landed](#what-landed)

## Related docs

- [`../../spec/shim/shim-coverage.md`](../../spec/shim/shim-coverage.md) — the
  canonical, kept-current record this entry's findings were moved
  into (corpus results, full symbol list, the NSS proof,
  implementation notes).
- [`../../reference/syscall-boundary.md`](../../reference/syscall-boundary.md) —
  what's left (raw `syscall()`, static binaries, libc-internal NSS
  opens), which belongs to the tracer, not this shim.

## What landed

The code review in
[#1](https://github.com/jronminh/deb-native/issues/1) listed what the
libc-level shim could not yet see. `521cc73` closed the review items
(`unlink()` bug; `lstat`; the missing `*64` names; fortified `__open_2`/
`__openat_2`/`__open64_2`; `statfs`/`statvfs`; `dlopen`/`dlmopen`; AF_UNIX
`bind`/`connect`). A second pass, once the corpus was measured against a
decided scope, closed the remaining genuinely-imported-but-uncovered
symbols and tested the NSS question to a conclusion.

**Full details moved into [`shim-coverage.md`](../../spec/shim/shim-coverage.md)** — the
corpus results, the complete symbol list, the NSS proof (not redirectable
at the libc layer — upstream glibc design, not a Termux packaging bug), and
the implementation notes for `mkstemp`'s in-place template and
`posix_spawn`'s own wrapper — since that doc is the canonical, kept-current
record of shim coverage. What's left all belongs to the tracer (raw
`syscall()`, static binaries, libc-internal NSS opens) — see
[`syscall-boundary.md`](../../reference/syscall-boundary.md).
