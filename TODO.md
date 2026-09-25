# TODO / roadmap

Ordered the way `sudo-less` orders its own work (`docs/design.md`, "Order
of work"), and judged by its criterion: **per-section coverage from a
random sample**, not feature count. See
[`docs/vs-sudo-less.md`](docs/vs-sudo-less.md) for the side-by-side diff
and [`docs/findings-runtime-and-base-2026-09-25.md`](docs/findings-runtime-and-base-2026-09-25.md)
for what just landed.

## Now / do first

- [ ] **Re-run the coverage survey on a fresh prefix** (sudo-less's
      yardstick; the base-env fix invalidates the old 33%).
      ```
      python3 scripts/sample-packages.py <packages-file>   # seed 20260925
      OUT=~/survey1 scripts/survey-apt.sh LIST.tsv
      ```
      Report: installed %, and the failure classes (unresolvable /
      maintainer-script / hardcoded-path / other). This decides whether
      the next build is run wrappers (#1) or the classifier (#2).

## Then, in order

- [ ] **Stage 4 run wrappers** (`prefix-wrap` equivalent) — the biggest
      unbuilt piece. For each binary a package puts on `PATH`, detect
      whether it needs path help (interpreter not present, interpreter's
      compiled-in module path, `ldd`-missing lib, hardcoded `/usr /etc
      /opt` path) and generate a wrapper. Build on
      [`docs/design-static-wrappers.md`](docs/design-static-wrappers.md);
      trigger via [`docs/design-hooks.md`](docs/design-hooks.md) (apt
      `DPkg::Post-Invoke` + a `dpkg` wrapper), not a source patch.
- [ ] **Stage 2 classifier / refusal** (`prefix-check` equivalent) — read
      each `.deb` before dpkg runs; classify scope (in / admin's / never)
      and mechanism (none / env / shim), flag unsafe maintainer scripts,
      and refuse a "never" package **before** dpkg can wedge the prefix.
- [ ] **Stage 1 sync on `apt update`** — keep the prefix's view of
      Termux's `*-glibc` packages fresh (`APT::Update::Post-Invoke-Success`),
      so a Termux upgrade doesn't leave stale seeds / "held broken
      packages".
- [ ] **Patch B: two-layer database, soname-based** — replace
      `scripts/native-seed.sh`'s hand-written ~10-entry name table with
      matching a `.deb`'s `Depends:` against installed `*-glibc` packages'
      `.so` SONAMEs (`docs/design-native-deps.md`, "Open work"). Small,
      directly improves install success.
- [ ] **Stage 4 integration** — launchers/icons/desktop DB (mostly N/A on
      Android; do only what Termux needs).
- [ ] **Direction 3: services on `termux-services` (runit)** — translate a
      package's systemd unit into a runit `run` script
      (`docs/services-research.md`). The service *view* (config/state at
      `/etc/foo`, `/var/lib/foo`) is the hard part and shares Direction 2's
      unsolved gap.
- [ ] **State + `explain` + `doctor`** — record per package its scope,
      mechanism and the wrappers it got, so decisions are explainable and
      removable; `sudo-less` doesn't have these either.

## Quick wins

- [ ] no-op shim for `dpkg-statoverride` (Termux's `dpkg` doesn't ship it;
      `ca-certificates` logs `command not found` but still reaches `ii`).
- [ ] refresh `README.md` status numbers once the survey (#Now) reports.

## Open, still-unsafe

- [ ] `--force-architecture` workaround for the archive-name mismatch
      (`arm64` vs `aarch64`) — flagged unsafe in
      `docs/findings-prototype-2026-09-25.md`; decide on a real fix.

## Blocked / impossible on this device

Keep these out of scope, they cannot be built without namespaces (this
kernel: `unshare(CLONE_NEWUSER)` = `EINVAL`, FUSE closed):

- install-view / service-view isolation (hidden `$HOME`, empty `/run`);
- `prefix-sandbox`'s seccomp + namespace isolation;
- other-architecture (i386) loaders and `Multi-Arch` skew;
- setuid/setgid and file capabilities;
- a TUN device, firewall rules, raw sockets.
