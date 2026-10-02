# Findings: finishing the libc-level shim, and closing the measured gaps (2026-09-26)

> Template: [`templates/docs.template.md`](../../../templates/docs.template.md)
> (fix the relative path to match this file's depth). Read a doc's
> summary and table of contents below before its sections, and read
> its directory's own `README.md` first to confirm this is the right
> doc to open. Create a new doc, instead of extending an existing
> one, when the content is a distinct kind of writing — a new spec
> topic, a new one-off investigation, or a new guide — not just a
> long addition to what a doc already covers.

**Impact: Repo change.** `521cc73` closed every item a code review
flagged; what's left belongs to a different layer entirely (the
tracer), already tracked there.

## Contents

- [What landed](#what-landed)

## Related docs

- [`../../spec/shim-coverage.md`](../../spec/shim-coverage.md) — the
  canonical, kept-current record this entry's findings were moved
  into (corpus results, full symbol list, the NSS proof,
  implementation notes).
- [`../../spec/syscall-boundary.md`](../../spec/syscall-boundary.md) —
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

**Full details moved into [`shim-coverage.md`](../../spec/shim-coverage.md)** — the
corpus results, the complete symbol list, the NSS proof (not redirectable
at the libc layer — upstream glibc design, not a Termux packaging bug), and
the implementation notes for `mkstemp`'s in-place template and
`posix_spawn`'s own wrapper — since that doc is the canonical, kept-current
record of shim coverage. What's left all belongs to the tracer (raw
`syscall()`, static binaries, libc-internal NSS opens) — see
[`syscall-boundary.md`](../../spec/syscall-boundary.md).
