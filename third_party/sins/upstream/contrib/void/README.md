# Void Linux (xbps-src) — minimal SINS builds

Void users often want **only** `systemctl` + `libsystemd.so` for AUR-style scripts or a single app, without timers/sockets/cgroups.

## From a git checkout

```sh
git clone https://github.com/Spinty-dev/SINS.git && cd SINS

# smallest
./build.sh --profile minimal
doas install -Dm755 build/systemctl /usr/local/bin/systemctl
doas install -Dm755 build/libsystemd.so.0 /usr/local/lib/libsystemd.so.0
doas ln -sf libsystemd.so.0 /usr/local/lib/libsystemd.so

# desktop-ish (D-Bus + notify; run sins-daemon under runit)
./build.sh --profile de
# install binaries + share/dbus-1/system.d/org.freedesktop.systemd1.conf + runit sv (see PKGBUILD paths)
```

Exact tag set for scripts:

```sh
./build.sh --print-tags --profile server   # → dbus,timers,cgroups
SINS_TAGS=dbus,notify ./build.sh
```

Optional smaller binaries: `SINS_STRIP=1 ./build.sh --profile minimal`

## xbps-src template sketch

In `srcpkgs/sins/template` (adjust version, distfiles, `build_style=go` if you prefer):

- Run `./build.sh` with `SINS_PROFILE` exported (or `SINS_TAGS`).
- In `do_install`, copy only files that exist under `build/` (same idea as the Arch `PKGBUILD` `package()` guards).

Depends: `runit`, `libelogind`; use `depends` for `dbus` only when the template builds a profile that includes `sins-daemon`.
