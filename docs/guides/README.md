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
  library-search convention, `LD_LIBRARY_PATH` (the prefix's `ld.so.cache`
  resolves its dirs, and the loader honours a caller's entries).
- `fixing-runtime-edge-cases.md` — a runtime problem hits the prefix (a
  path outside it, a library not found, a call past the shim): the decision
  table from symptom to the cheapest layer (shim, `ld.so.preload`/
  `ld.so.conf`+`ldconfig`, dynamic tags, launchers, tracer) and the two
  cases that force a glibc rebuild.
