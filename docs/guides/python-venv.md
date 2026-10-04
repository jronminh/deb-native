# Guide: Python + pip with real manylinux wheels

> Template: [`templates/docs.template.md`](../../templates/docs.template.md)
> (fix the relative path to match this file's depth). Read a doc's summary
> and table of contents below before its sections, and read its
> directory's own `README.md` first to confirm this is the right doc to
> open. Create a new doc, instead of extending an existing one, when the
> content is a distinct kind of writing — a new spec topic, a new one-off
> investigation, or a new guide — not just a long addition to what a doc
> already covers.

Install Debian's own `python3`/`pip` into a deb-native prefix and use a
venv from it to install ordinary PyPI packages, including ones with
compiled extensions (`pydantic-core`, `cffi`, …). Done and working: this
is the escape hatch for any Python project that needs a wheel Termux's
own Bionic Python cannot install because there is no `manylinux`/
`android` wheel and no feasible from-source build (Rust targeting
`aarch64-linux-android` is the common failure). Status: working, three
reproducible rough edges documented below, each with its fix.

## Contents

- [Why Termux's own Python hits a wall](#why-termuxs-own-python-hits-a-wall)
- [Setup](#setup)
- [The `pip` launcher quirk](#the-pip-launcher-quirk)
- [`pytest` needs `dn-trace`](#pytest-needs-dn-trace)
- [`ctypes.CDLL`/`cffi` loading your own `.so` by bare name](#ctypescdllcffi-loading-your-own-so-by-bare-name)
- [Status](#status)

## Related docs

- [`../spec/design.md`](../spec/design.md) — `dn-shell`, the prefix layout,
  and the mechanism this guide assumes.
- [`tailscale.md`](tailscale.md) — the other worked guide in this
  directory, a tracer-boundary case rather than a shim one; this guide
  stays entirely inside the shim-covered, dynamic-binary happy path.

## Why Termux's own Python hits a wall

Termux's `python3` is built for Bionic (`android-arm64`), not glibc
`linux-aarch64`/`manylinux`. Pure-Python packages install fine with
`pip`/`uv`, but any package shipping a compiled extension has no
prebuilt wheel for that platform tag. `pip`/`uv` then fall back to
building from source, which for a Rust-backed package
(`pydantic-core`, and anything depending on `pydantic>=2`) fails like
this:

```
error: Failed to build `pydantic-core==2.46.5`
Computed rustc target triple: aarch64-unknown-linux-android
Target triple not supported by rustup: aarch64-unknown-linux-android
```

`rustup` (what `maturin`'s build backend shells out to) has no
`aarch64-linux-android` target at all — Termux's own `rust` package
builds for Android directly without going through `rustup`, so
`rustup target add` cannot fix this even if installed. Chasing a
working from-source Rust-for-Android toolchain is a dead end for this
case; the fix is to stop needing one.

## Setup

Run Python from **inside the prefix** instead — it is Debian's real
`glibc` build, so PyPI's ordinary `manylinux_2_17_aarch64`/
`manylinux2014_aarch64` wheels (which is most of them, including
`pydantic-core`) install as prebuilt binaries, no compiler involved.

```sh
dn-shell -c "apt update && apt install -y python3 python3-venv python3-pip python3-dev"
```

Then, per project, make a venv and use it exactly like any other:

```sh
cd ~/your-project
dn-shell -c "cd $PWD && python3 -m venv .venv-dn"
dn-shell -c "cd $PWD && .venv-dn/bin/python3 -m pip install pydantic fastapi sqlalchemy ..."
```

Run the project itself the same way, e.g.
`dn-shell -c "cd $PWD && .venv-dn/bin/python3 -m uvicorn app:app"`.

`uv` is Termux's own (Bionic) binary and cannot target this venv's
interpreter for anything needing a compiled wheel — for a project that
hits this wall, `pip` inside the prefix's venv replaces it; `uv` is
still fine for a project whose dependencies are pure Python.

## The `pip` launcher quirk

Calling the venv's `pip` script directly fails, reproducibly, on **any**
command that builds pip's network session (`install`, `download`, …) —
even just `pip install --upgrade pip`:

```
subprocess.CalledProcessError: Command '('uname', '-rs')' returned non-zero exit status 1.
```

The traceback bottoms out in `pip._vendor.distro`, which shells out to
`uname -rs` as part of building the `User-Agent` string. Confusingly,
`uname -rs` run any other way inside the prefix — directly, or via the
exact same `subprocess.check_output` call from a one-off `python3 -c`
— succeeds. The failure is specific to invoking the installed
`.venv-dn/bin/pip` **launcher script** (a `#!...python3` shebang
script); not reproduced, not yet root-caused (candidate: how the
shim/launcher resolution handles a script invoked by its own long
translated path rather than by `python3 -m`).

**Workaround**: always invoke pip as `python3 -m pip`, never as
`pip`/`pip3` directly, inside the prefix:

```sh
.venv-dn/bin/python3 -m pip install <packages>   # works
.venv-dn/bin/pip install <packages>              # fails every time
```

## `pytest` needs `dn-trace`

Running `pytest` (even a trivial pure-Python test file, no compiled
extensions involved) through the venv's launcher dies with `Bad system
call` (`SIGSYS`), the same class of gap as the `ldconfig -r` one noted in
`docs/log/android-seccomp-audit.md` — some syscall `pytest`'s own
machinery makes (collection, capture, or cache handling; not
root-caused) isn't in the shim's covered set and isn't auto-routed to
the tracer. `-p no:cacheprovider`, `-p no:faulthandler`, and `-s` (no
capture) do not avoid it, so it isn't any one obvious plugin.

**Workaround**: run `pytest` explicitly under `dn-trace`:

```sh
~/deb-native/tracer/dn-trace -- .venv-dn/bin/python3 -m pytest tests/
```

This is slower (every syscall goes through `ptrace`) but has run a full
suite (protocol-layer tests: signing, canonicalization, tag
normalization, Pydantic models) with no further issues. Worth
revisiting once `pytest`'s exact offending syscall is identified — it
may be a small, fixable shim/`fakesyscall.json` gap rather than
something that needs the tracer for every run.

## `ctypes.CDLL`/`cffi` loading your own `.so` by bare name

Most PyPI wheels with compiled extensions (`pydantic-core`, `cffi`'s own
`_cffi_backend`, …) need nothing special — they're self-contained,
either linking only against libraries already in the prefix's `ld.so.cache`
(`$DN/usr/lib`, `$DN/usr/lib/aarch64-linux-gnu`) or carrying their
own bundled `.so` deps with an `-rpath $ORIGIN`-style reference baked in
at build time (`auditwheel`'s doing, not this project's). Confirmed
working with no extra steps in this guide's own test suite.

The one case that *does* need help: code that calls
`ctypes.CDLL("libfoo.so")` or `cffi`'s `dlopen` with a **bare name**
(not an absolute path) for a library you built yourself, not installed
via `apt`. `dlopen()` by bare name consults the process's
`LD_LIBRARY_PATH`, which for anything launched in the prefix starts from
`ld.so.cache`'s directories, then the caller's entries merged in
(`docs/guides/gcc-glibc-dev.md` has the full story, found building an
unrelated project's CLI). Confirmed:

```python
import ctypes
ctypes.CDLL("libadd.so")  # OSError: cannot open shared object file
```

**Fix**: set `LD_LIBRARY_PATH` before launching Python, the standard
glibc way and the same one variable as for a plain C program:

```sh
LD_LIBRARY_PATH=/path/to/your/libs .venv-dn/bin/python3 your_script.py
```

Confirmed: the exact `ctypes.CDLL` call above succeeds with this set,
no code change needed.

## Status

Working end to end: a `pydantic`/`fastapi`/`sqlalchemy`/`pynacl`-class
dependency set installs cleanly via prebuilt wheels through this path,
no Rust and no from-source build needed. The `pip` launcher quirk, the
`pytest`/`dn-trace` requirement, and the `ctypes`/`cffi` bare-name case
above are known, worked-around rough edges, not blockers.
