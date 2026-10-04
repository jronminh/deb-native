# Findings: a real server stack (sockets, SQLite WAL, uvloop, Alembic) works unmodified — credit Termux's glibc, not 0.5.0 (2026-10-02)

<!-- template: templates/docs.template.md -->

**Impact: Isolated.** A confirmation, not a fix — no code changed
here. Kept as a named baseline for later regression-checking.

Building `vdir` (an unrelated external project, a small signed-registry
server) against the Debian-glibc Python venv from
[`docs/guides/python-venv.md`](../../guides/python-venv.md), stage 3 of its
own roadmap needed three syscall-heavy things this project had never
exercised before: a real TCP socket (`uvicorn`'s `bind`/`listen`/
`accept`), SQLite in WAL mode (`mmap`, file locking, via both `sqlite3`
and SQLAlchemy), and Alembic (file generation, its own subprocess-free
CLI). Smoke-tested each directly (no `dn-trace`) before writing any
real code for that project:

## Contents

- [What was tested](#what-was-tested)
- [Important attribution](#important-attribution-not-to-overclaim-this-projects-own-work)

## Related docs

- [`../../guides/python-venv.md`](../../guides/python-venv.md) — the
  venv setup this test ran against, including the unrelated `pytest`/
  `dn-trace` gap this entry's checks did *not* hit.
- `docs/reference/android-platform.md` — the `kernel-features.h.patch` note
  this entry explains is about 0.5.0's own-glibc build, not today's
  runtime.

## What was tested

- Raw `socket.bind`/`.listen` on `127.0.0.1` — fine.
- `sqlite3` and SQLAlchemy, `PRAGMA journal_mode=WAL`, insert, commit,
  select — fine.
- A real `uvicorn.Server` (`FastAPI`, `httpx` client), including with
  `loop="uvloop"` and `http="httptools"` explicitly (both pulled in by
  `uvicorn[standard]`, both C extensions) — started, served a real HTTP
  request, shut down clean. No `SIGSYS`.
- `python3 -m alembic init` — generates its template tree without
  issue.

None of this needed `dn-trace`, unlike the `pytest` gap in
`docs/guides/python-venv.md`.

## Important attribution, not to overclaim this project's own work

The prefix's active `libc6` today is still
Termux's own glibc side-install (`glibc-packages`), a mature, widely-used
package — not yet 0.5.0's own-built `libc6`, which per `TODO.md` is
"written, forked, and validated... but not yet packaged as the prefix's
real `libc6`". The `kernel-features.h.patch` note in
`docs/reference/android-platform.md` ("no
separate `accept`/`recv`/`send` syscalls — needed for sockets to work at
all") describes a fix already folded into *that* own-glibc patch set for
when it eventually becomes the default; it says nothing about today's
runtime, which was never missing it.

So this is not a deb-native capability newly proven — it is the
existing Termux glibc side-install doing what it has always done,
confirmed under this project's shim/tracer layer via a real external
project instead of a synthetic probe. Worth keeping as a **named
baseline**: once 0.5.0's own `libc6` replaces the side-install as the
prefix default, these four checks (socket, SQLite WAL, uvloop/httptools,
Alembic) are a fast regression smoke test to confirm nothing those
patches touch (syscall emulation, `fakesyscall.json` buckets) broke
networking or `mmap`-backed I/O for an ordinary Python server stack.
