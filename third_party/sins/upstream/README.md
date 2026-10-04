# SINS - SINS Is Not Systemd

**SINS** is a modular, lightweight compatibility layer that bridges the gap between `runit` and `systemd`. It allows you to run `systemd`-dependent software (like Nginx, Docker, or GNOME services) on `runit`-based systems by providing a `systemctl` shim and background daemons for D-Bus, Notify signals, and Cgroups resource management.

---

[English](README.md) | [Русский](README.ru.md)

---

## What SINS is (and is not)

SINS is a **compatibility shim for runit-based systems**, not systemd. Many applications work unchanged; anything that requires real **systemd features** (user@ services, portable images, full journal namespaces, cgroup APIs from systemd, and similar) may still fail — see the matrices below.

### Compatibility matrix

**systemctl** (partial parity — aimed at installer/script callers):

| Command / behavior | Level |
|--------------------|--------|
| start, stop, restart, reload, status, enable, disable | Supported (maps to `sv` / symlink layout) |
| daemon-reload, show, cat, list-units, list-unit-files, is-system-running | Supported (simplified semantics vs systemd) |
| mask, unmask | Supported (system: `/etc/sins/masked`, user: `~/.config/sins/masked`) |
| try-restart, reload-or-restart, try-reload-or-restart, kill | Supported (best-effort `sv` / `ExecReload`) |
| preset, preset-all | No-op with documented exit success (no preset database) |
| `--user` start/stop/enable/disable | **Supported** — private runit tree under `~/.runit/` |
| Targets (graphical.target, multi-user.target) | Supported — `start`, `stop`, `isolate` commands |
| Everything else | Not planned unless someone contributes it |

**Unit files → run scripts**

| Feature | Level |
|---------|--------|
| ExecStart, ExecStartPre (shell-quoted `sh -c`) | Supported |
| Type=simple default, notify (sets `NOTIFY_SOCKET`), forking (waits on `PIDFile` if set) | Partial |
| Type=oneshot | Partial: starts and logs, but runit does not replicate exact systemd oneshot state transitions |
| Environment, EnvironmentFile (KEY=value lines), WorkingDirectory | Partial (see `pkg/runit/manager.go`) |
| User via `chpst -u` | Partial |
| Group, ambient capabilities, systemd drop-ins, slices | Not planned / ignored |

**D-Bus** (with `dbus` build tag):

| Area | Level |
|------|--------|
| `org.freedesktop.systemd1` Manager basics, introspection-oriented stubs | Partial |
| D-Bus service activation | Supported — auto-starts services from `/usr/share/dbus-1/services/` |
| Socket activation (`ListenStream`) | Supported — passes `LISTEN_FDS` to services |
| Setters on hostname/timedate/locale | **Fail with explicit errors** (no silent success) |

**libsystemd / journal**

| Area | Level |
|------|--------|
| Trampoline to elogind for most symbols | Supported where mapped |
| Missing symbol after `dlsym` | **Controlled abort + message** (no jump to NULL) |
| `sd_journal_*` file backend | Supported; wait/fd use **inotify** on the log directory when available |
| sins-journalctl | **New** — read logs in journalctl format |

### Community & packaging

- **Artix / runit**: SINS targets this workflow; tracking gaps publicly beats claiming full systemd parity.
- **Forum / wiki**: Open a thread on [Artix forums](https://forum.artixlinux.org/) or document quirks on the wiki so others can find supported-package lists — contributions welcome.

### Modules (optional build tags)

- **dbus**: D-Bus bridge for systemd-compatible names + service activation.
- **notify**: Notify socket support (`/run/systemd/notify`).
- **timers**: `.timer` scheduling daemon with full calendar spec support (`Mon *-*-01`, etc.).
- **sockets**: `.socket`-style activation helper with `LISTEN_FDS` passing.
- **cgroups**: Best-effort cgroup limits under `/sys/fs/cgroup/sins/`.
- **targets**: Target unit support (`graphical.target`, `default.target`).

### Desktop user checklist (KDE / Hyprland + AUR — “don’t think, just use repos”)

SINS covers the **`systemctl` + `libsystemd.so` + `org.freedesktop.systemd1`** slice. The rest of a comfortable desktop is **not** this repo — but this order keeps surprises low:

1. **Build/install**: `SINS_PROFILE=de` or `full` (see `build.sh --list-profiles`). Minimal (`systemctl` + shim only) is for headless experiments, not full Plasma.
2. **Session / seat**: **elogind** (and your display manager or wayland session) — required for KDE/Wayland login the same as on Artix without systemd.
3. **Policy / permissions**: **polkit** + your user in the right groups (`network`, `storage`, etc. per wiki) so GUI tools stop asking for impossible things.
4. **Portals & apps**: **xdg-desktop-portal** + at least one backend (**xdg-desktop-portal-kde**, **-gtk**, or **-wlr** for Hyprland) so Flatpak/Discord/file dialogs behave.
5. **Audio / devices**: **PipeWire** (or PulseAudio) and **BlueZ** as you would on Arch; not shimmed by SINS.
6. **Disks / automount**: **udisks2** + **udiskie** (or your automounter); they talk to the session and udev, not to PID1.
7. **Pacman**: keep **`IgnorePkg = systemd`** (and friends) as in the “Package management” section so updates do not replace the shim.

**`systemctl --user`**: Full support — user services run under separate runit tree (`~/.runit/sv` → `~/.runit/service`). Masking, enable/disable, start/stop all work. sins-daemon автоматически запускает runsvdir для user сервисов при `SINS_SESSION=1`.

AUR / `PKGBUILD` quirks (link tests, postinst): see [contrib/desktop/AUR-checklist.md](contrib/desktop/AUR-checklist.md). Quick smoke: `test/smoke_aur.sh` after `./build.sh --profile minimal`.

### CLI test matrix (release baseline)

The release gate for CLI compatibility is the smoke suite in `test/`:

- `test/smoke.sh` — journal shim, basic `systemctl` flow, optional D-Bus session smoke.
- `test/smoke_aur.sh` — linker/`pkg-config` sanity for AUR-style builds.
- `test/smoke_template_units.sh` — `@` template resolution and `daemon-reload` regeneration.
- `test/smoke_oneshot_forking.sh` — script generation assertions for `Type=oneshot` and `Type=forking`.
- `test/smoke_reload.sh` — `ExecReload` execution path.
- `test/smoke_user_mode.sh` — `--user` read-only behavior and mutation guardrails.
- `test/smoke_dbus_unit_props.sh` — `org.freedesktop.systemd1.Unit` properties (including boolean fields) and Manager error paths.

See [contrib/desktop/release-checklist.md](contrib/desktop/release-checklist.md) for go/no-go before tagging.

### Security notes (defense in depth)

SINS does **not** replace a full system hardening program, but the code aims to reduce common footguns:

- **Service names** passed to `sv` and used for paths are validated (no `/`, `..`, or shell metacharacters) — see `pkg/safeunit`.
- **Generated run scripts** quote `WorkingDirectory`, `PIDFile`, `chpst -e` paths, and `ExecReload` uses the same single-quote escaping as `ExecStart` to avoid command injection from unit files.
- **Environment keys** from `Environment=` / `EnvironmentFile` must look like safe identifiers so they cannot write outside the per-service `env/` directory.
- **System D-Bus policy** (`org.freedesktop.systemd1.conf`): non-root callers can introspect and call read-oriented `Manager` methods; **StartUnit / StopUnit / RestartUnit** are denied on the system bus (root policy unchanged). Adjust locally if you rely on non-root activation via the bus.
- **Unix activation / notify sockets** default to mode `0666` for Docker-style compatibility. Tighten with **`SINS_UNIX_SOCKET_MODE`** (e.g. `0660`) and **`SINS_NOTIFY_SOCKET_MODE`** (octal strings).

---

## 🐧 Supported Distributions

SINS is designed for **Linux** distributions running **runit** as the init system or service manager:
- **Artix Linux** (runit flavor)
- **Void Linux**
- **Arch Linux** (with custom runit setup)
- **Devuan** (runit flavor)

---

## � Quick Start (Artix runit)

```bash
# 1. Build (from regular user, no root needed for build)
./build.sh --profile full

# 2. Install binaries (as root)
sudo cp build/systemctl build/sins-daemon build/sins-journalctl /usr/bin/
sudo cp build/libsystemd.so.0 /usr/lib/

# 3. Install sins-daemon as runit service (as root)
sudo ./build.sh --install

# 4. Start daemon
sudo sv start sins-daemon

# 5. For user services (pipewire, etc.), add to ~/.bash_profile:
export SINS_SESSION=1
```

**That's it.** Now `systemctl start nginx`, `systemctl --user start pipewire`, etc. work.

---

## 🛠️ Building & Installation

### Dependencies
- **Go** (1.25+ recommended)
- **GCC** and GNU `ld` — to build the `libsystemd.so.0` shim (Linux **x86_64** only). **You do not need systemd or libsystemd installed** to compile; the real symbols are resolved at runtime via elogind.
- **Runit** and **D-Bus** — runtime dependencies on target systems, not required on a pure build host.

On non-x86_64 hosts, `build.sh` still produces the Go binaries but skips the shared library.

### Maintainer: updating the exported ABI
Edit `pkg/libsystemd/libsystemd.map`, then regenerate trampolines:

```bash
python3 scripts/sync_stub_jumps.py
```

Or `SINS_SYNC_STUB=1 ./build.sh` before compiling the stub (requires Python 3).

### Userspace journal (sd_journal)
SINS implements **`sd_journal_*`** on top of a single append-only binary file (not systemd-journald). Writers and readers use the normal libsystemd API; data is stored under:

- `$SINS_JOURNAL_FILE` if set, else
- `/var/log/sins-journal/journal.sins` (if writable), else
- `/tmp/sins-journal/journal.sins`

To dump the log for debugging:

```bash
python3 scripts/sins-journal-cat.py
python3 scripts/sins-journal-cat.py /path/to/journal.sins
```

The package installs `/etc/logrotate.d/sins-journal` (from `contrib/logrotate/sins-journal`). On fresh setups, ensure `/var/log/sins-journal` exists and is writable for writers that cannot fall back to `/tmp` (the `sins.install` script adjusts ownership when the `adm` group exists).

### 1. Profiles (recommended for Artix / Void minimalism)

```bash
./build.sh --list-profiles          # table: minimal, dbus, de, server, full
./build.sh --profile minimal        # systemctl + libsystemd.so only (smallest)
./build.sh --profile de             # dbus + notify (typical Plasma/GNOME stack)
./build.sh --profile server         # dbus + timers + cgroups (no socket activator)
./build.sh --full                   # everything (former “choice 0”)

# Exact Go tags (overrides profile):
SINS_TAGS=dbus ./build.sh
./build.sh --tags dbus,notify

# Smaller binaries:
SINS_STRIP=1 ./build.sh --profile minimal

# Scripts / templates: print resolved tag list
./build.sh --print-tags --profile de   # → dbus,notify
```

Non-interactive default is **`minimal`** (no D-Bus daemon) unless you set `SINS_PROFILE`, `SINS_TAGS`, or legacy `SINS_CHOICE`.

Void: see [contrib/void/README.md](contrib/void/README.md).

### 2. Interactive build

```bash
chmod +x build.sh
./build.sh
```

Prompts for a **letter profile** (`m`/`d`/`e`/`s`/`f`) or legacy numeric `SINS_CHOICE` (`0` = full, `1,4` = dbus+notify, etc.).

### 3. Legacy / CI

```bash
SINS_CHOICE=0 ./build.sh            # full stack (same as --full)
./build.sh --verify                 # compile check: all tags + no tags + stub
```

Continuous integration runs `./build.sh --verify`, `go test ./...`, a **Go build tag matrix**, `build-profiles` (minimal/de), and the full smoke suite (`test/smoke*.sh`).

### 4. Installation (Arch/Artix)
For AUR-compatible distributions, use the provided `PKGBUILD` (default **`SINS_PROFILE=full`**). Minimal package from the same `PKGBUILD`:

```bash
SINS_PROFILE=minimal makepkg -si
```

For a desktop-oriented build without socket activation: `SINS_PROFILE=de makepkg -si`.

### 5. Package Management (Arch Linux)
To prevent `systemd` updates from overwriting the SINS shim, it is recommended to add `systemd` to `IgnorePkg` in your `/etc/pacman.conf`:
```ini
[options]
IgnorePkg = systemd
```

---

## 🧬 Usage

### Managing Services
Use the `systemctl` binary as usual:
```bash
systemctl start nginx
systemctl status nginx -f
```

### SINS Daemon
If you enabled D-Bus or Notify support, ensure `sins-daemon` is running in the background. You can set it up as a runit service itself:
```bash
# Create runit service for sins-daemon
mkdir -p /etc/runit/sv/sins-daemon
echo -e "#!/bin/sh\nexec sins-daemon" > /etc/runit/sv/sins-daemon/run
chmod +x /etc/runit/sv/sins-daemon/run
ln -s /etc/runit/sv/sins-daemon /var/service/
```

---

## 📜 License
MIT
