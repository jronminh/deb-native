# SINS pre-release go/no-go checklist

Use this checklist before creating a release tag.

## 1) Build and tests (must pass)

```bash
./build.sh --verify
go test ./...
SINS_PROFILE=minimal ./build.sh
SINS_PROFILE=de ./build.sh
SINS_PROFILE=full ./build.sh

chmod +x test/*.sh
test/smoke.sh
test/smoke_aur.sh
test/smoke_template_units.sh
test/smoke_oneshot_forking.sh
test/smoke_reload.sh
test/smoke_user_mode.sh
test/smoke_dbus_unit_props.sh
```

## 2) VM/manual sanity (desktop-first)

- `systemctl status nginx` / `systemctl status docker` show useful logs and non-empty status details.
- `busctl` checks:
  - `ListUnits` returns expected services.
  - Unit properties (`CanStart`, `CanStop`, `CanReload`) return boolean variants.
- Socket and timer smoke on target VM:
  - `.socket` activation starts target service.
  - `.timer` daemon triggers expected unit.
- `--user` behavior:
  - `status/show/cat` work on user unit paths.
  - mutation commands (`start/enable/mask/...`) fail with explicit guidance.

## 3) PATH sanity (avoid stale binaries)

Ensure the system does not pick an old `systemctl` binary from a higher-priority path:

```bash
command -v systemctl
ls -l /usr/bin/systemctl /usr/local/bin/systemctl 2>/dev/null
```

If both exist, keep them in sync or remove the stale one.

## 4) Docs and packaging sanity

- README + README.ru include known limits (`Type=oneshot` partial, `--user` scope).
- `PKGBUILD` profile behavior is documented and install scripts are consistent.
- CI workflow includes verify, tests, and smoke suite.
