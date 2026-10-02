# docs/guides/

> Template: [`templates/readme.template.md`](../../templates/readme.template.md). Read this
> file before touching anything in this directory or guessing a
> file's purpose from its name alone. Add a `README.md` like this one
> whenever a new directory holds more than a couple of files that
> aren't self-explanatory from their names alone.

How-to for a specific, one-off case — not a spec of the project itself.

- `tailscale.md` — install Tailscale's Debian `arm64` package into a
  prefix and run it: the static-daemon goal (userspace networking),
  the project's target case for the tracer.
- `python-venv.md` — install Debian's own `python3`/`pip` into a prefix
  and use a venv from it to get real `manylinux` wheels (e.g.
  `pydantic-core`) that Termux's Bionic Python can't install or build;
  the `pip` launcher quirk (`python3 -m pip` workaround) and the
  `pytest`-needs-`dn-trace` gap.
- `gcc-glibc-dev.md` — compile, link, and run C code with `apt install
  gcc` inside the prefix: `make`, shared libraries, and the one
  library-search convention, `LD_LIBRARY_PATH` (`ld-dn` sets it, prefix
  dirs first, and merges the caller's entries after).
