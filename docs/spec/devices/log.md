# Log and console devices

Status: **proposal** (2026-10-19).  Small device models for the
[device table](../devices.md).  They are **data** devices: they let a program
write messages and read its own, not the kernel's.

## `/dev/kmsg`

| | |
| --- | --- |
| guest path | `/dev/kmsg` |
| kind | `char` |
| `st_rdev` | `1:11` |
| model | a per-prefix message ring |
| access | write; read returns only messages written by this prefix |

- `write` takes the kernel's format (`<level>text` or plain text) and appends a
  record to the prefix ring; the host may mirror it to the app log.  A line
  longer than the record limit is truncated as the kernel would.
- `read` returns records in `dmesg`-like format with a monotonic sequence; it
  never returns the host kernel's log.  `dmesg` therefore shows the prefix's
  messages, not the phone's.
- `poll` reports readable when a new record exists; the ring is bounded and
  overwrites the oldest, like the kernel buffer.
- ioctls are not promised; an unknown one is `ENOTTY`.
- The device is not a control channel: there is no `sysrq`, no log-level
  ioctl, and no way to reach Android's log from the guest except through the
  host's own bridge.

## `/dev/console`

Android has no VT and no kernel console.  A console is a TTY, so the only
honest implementation is a **PTY** whose master the host console plane owns:

- If the host provides a console PTY, `/dev/console` is a bind/row to its slave
  (a real TTY, with termios and `SIGWINCH`); writes appear in the app's console
  plane, reads come from its input.
- If it does not, `/dev/console` is refused (`ENODEV`), not a fake TTY.  A
  program that writes a banner to `/dev/console` gets a clean error; it does not
  get a socket dressed as a terminal.
- `/dev/tty0..N` (virtual consoles) are refused: there is no VT layer to back
  them.  `/dev/tty` keeps its kernel meaning: the process's controlling
  terminal, if it has one.

The console model is deliberately separate from the display backend: a GUI
failure can still leave the console plane working
([`../host.md`](../host.md), "Display and input").

## Pass-through, not synthesis

- `/dev/null`, `/dev/zero`, `/dev/full`, `/dev/random`, `/dev/urandom`: the
  host's nodes through the `/dev` row.  They are kernel truth; no model.
- `/dev/ptmx`, `/dev/pts/*`: see [`pty-utmp.md`](../pty-utmp.md).
- `/dev/kmsg` is the one node where the host's answer would leak the phone's
  kernel log, so it is synthesized.

## Tests

- `echo '<6>hello' > /dev/kmsg`, then `dmesg` in the prefix shows it and does
  not show host records.
- Ring wrap; long line truncation; `poll`.
- Console: a write reaches the app console; a read receives input; `isatty`,
  `tcgetattr`, `TIOCSWINSZ`; with no console PTY, open is `ENODEV`.
- `/dev/tty0` is absent or refused.

## Milestones

| | scope |
| --- | --- |
| **L1** | `/dev/kmsg` write + ring + read/poll |
| **L2** | `/dev/console` via a host console PTY, if the host has one |

## Non-goals

- The host kernel log, `sysrq`, or log-level control.
- A VT (`/dev/ttyN`), `fbcon`, or a kernel console.
- A syslog daemon; the ring is not a service.

## Open questions

1. **Host mirror**: should `/dev/kmsg` writes always go to the app log, or only
   when a setting asks?
2. **Record format**: exact `dmesg` fields (facility, seq, timestamp) that
   `dmesg` in trixie expects.
3. **Console PTY ownership**: who allocates it, the app or the runtime, and
   what happens when the Surface/console plane is backgrounded?
4. **Refusal path**: is `ENODEV` at `open` better than a node that exists but
   fails on write?

## Sources

The kernel `/dev/kmsg`/`dmesg` documentation and the `printk` record format;
the host console is the PTY/console plane in
[`../pty-utmp.md`](../pty-utmp.md) and [`../host.md`](../host.md).
