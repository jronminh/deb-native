# third_party/sins/

<!-- template: templates/readme.template.md -->

A vendored copy of [`Spinty-dev/SINS`](https://github.com/Spinty-dev/SINS)
(MIT) -- a systemd-on-runit compatibility layer -- reduced to the `systemctl`
shim deb-native's services layer needs. `upstream/` is a pristine copy;
[`build.sh`](build.sh) applies [`patches/`](patches/) and prunes the desktop
modules, producing a static `arm64` `systemctl` whose paths resolve inside a
userland prefix (`DN_INSTDIR`) rather than the real `/etc`, `/run`, `/sys`.
That source-level redirect is why a static Go binary works here at all: the
`LD_PRELOAD` shim cannot see it. See
[`../../docs/log/findings/systemctl-on-runit-prior-art.md`](../../docs/log/findings/systemctl-on-runit-prior-art.md)
for why SINS was chosen.

- [`UPSTREAM`](UPSTREAM) — pinned repo, commit, and license of the vendored source.
- `upstream/` — pristine SINS at that commit; do not edit, extend `patches/` instead.
- `patches/` — our changes, applied in filename order by `build.sh`.
- [`build.sh`](build.sh) — copy `upstream/`, apply `patches/`, prune, build `systemctl`.
