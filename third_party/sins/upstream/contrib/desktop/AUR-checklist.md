# AUR / `PKGBUILD` expectations vs SINS

Typical friction points when a package assumes **real systemd**:

| Expectation | With SINS (`SINS_PROFILE=de` or `full`) |
|-------------|----------------------------------------|
| `systemctl` exists | Yes (`/usr/bin/systemctl` from this project) |
| `pkg-config --exists libsystemd` | **Yes** — PKGBUILD installs `libsystemd.pc` to both `/usr/lib/pkgconfig/` and `/usr/share/pkgconfig/` |
| postinstall calls `systemctl daemon-reload` / `try-restart` | Shim implements common commands; logs improve with `systemctl status` (runit + logs) |
| user units (`systemctl --user`) | **Full support** — separate runit tree under `~/.runit/`, see README for `SINS_SESSION=1` setup |
| hard dependency on **systemd** PID1 | Not satisfied — this is intentional; use Artix wiki / IgnorePkg as documented |

Quick link test after `makepkg` or `./build.sh`:

```bash
ldd /usr/lib/libsystemd.so.0   # or build/libsystemd.so.0
pkg-config --libs libsystemd    # may need a .pc from libelogind or a one-liner -L/usr/lib -lsystemd
```

| `sd_booted()` check | **Returns 1 (true)** — SINS reports as running under systemd |
| `systemd-analyze` | **Stub provided** — supports `--version` and `--help`; other commands exit 1 |
| `/run/systemd/system` | Created by post_install hook for runtime compatibility |

If an AUR build fails on version checks or needs `systemd-analyze` features beyond the stub, patch or report upstream.
