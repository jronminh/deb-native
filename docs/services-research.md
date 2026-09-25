# Direction 3 (research): services without systemd

Status: **research only, no design yet.** This doc records what's known and
what's still open — not a plan to implement against.

## What sudo-less does

`docs/services.md` in sudo-less (not yet read in full — follow-up) covers
translating a package's systemd unit into a `systemd --user` unit that runs
inside a "service view": a private mount-namespace view built fresh per
start, so the daemon reads its config from `/etc/foo/foo.conf` and writes
state to `/var/lib/foo` exactly as it would on real Debian, landing in the
prefix. `$XDG_RUNTIME_DIR/sudo-less/run` stands in for `/run`.

## Why it doesn't transfer

Two independent blockers, not one:

1. **No systemd on Termux at all** — no PID 1 systemd, no `systemctl`, no
   user manager, no unit files, no cgroups in the relevant sense. Termux's
   native service supervisor is [`termux-services`](https://github.com/termux/termux-services),
   built on `runit`: services are directories under
   `$PREFIX/var/service/<name>/run` (an executable script `runsv`
   supervises), enabled/disabled via `sv-enable`/`sv-disable`.
2. **No service view** — even if a runit script stood in for the systemd
   unit, the config/state/`/run` redirection sudo-less's service view
   provides depends on the same blocked mount-namespace mechanism as
   Direction 2 is working around. A runit script alone doesn't solve path
   resolution.

## Open questions (not yet answered)

- Does a `.deb` package that ships a systemd unit under
  `/lib/systemd/system/*.service` carry enough information in that unit
  file (`ExecStart=`, `ExecStop=`, `User=`, environment) to mechanically
  generate a runit `run` script, the way sudo-less mechanically generates a
  user unit? Needs reading actual systemd unit files from real packages to
  judge — not researched yet.
- Whether Direction 2's static wrappers (env vars for config/data paths)
  are enough for a *daemon* the same way they might be for a CLI tool, or
  whether daemons disproportionately fall into the "hardcoded absolute
  path, no env var" bucket `design-static-wrappers.md` flags as unsolved —
  daemons reading `/etc/<name>/<name>.conf` directly is extremely common
  and was one of `view.md`'s own motivating examples (`redis.conf`). If so,
  Direction 3 may be blocked on the same open problem as Direction 2's
  gap, not just on "no systemd".
- Whether `termux-services`/runit's process supervision model (long-running
  foreground process under `runsv`, restart on exit) is even the right fit
  for packages that assume `systemd`'s notify/socket-activation protocols
  (`Type=notify`, `sd_notify()`) — a package relying on those would need
  either a shim for `sd_notify` or exclusion, unresearched which is more
  common.

## Non-conclusion

This direction is **not** "port sudo-less's service view to runit" — that
undersells the problem, since the actual hard part (path resolution for a
daemon that expects `/etc/foo` and `/var/lib/foo` to be real) is shared
with Direction 2's unsolved gap, not something runit vs. systemd changes
either way. Next step is reading sudo-less's `docs/services.md` in full and
picking one real daemon package to trace by hand (what paths does it read,
what would a runit script + Direction 2's wrapper actually cover) before
writing any design here.
