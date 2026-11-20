# Input: evdev and uinput

Status: **proposal** (2026-10-19).  Device models for the
[device table](../devices.md), after fbdev.  They are **data** devices: a
program can read events or create a virtual input device inside the prefix; it
cannot inject into Android.

```
app (touch/keys/mouse) ──┐
guest /dev/input/eventN ─┼─> evdev provider ──> guest readers
guest /dev/uinput ───────┘                     (libinput, evtest, X)
```

## evdev

### The nodes

| | |
| --- | --- |
| guest path | `/dev/input/eventN` |
| kind | `char` |
| `st_rdev` | `13:N` (`INPUT_MAJOR`) |
| model | a fixed set of devices created at boot |
| access | read-only; `EVIOCGRAB` is supported |

v1 creates the devices the host declares (for example `keyboard`, `mouse`,
`touch`), with stable minor numbers.  Hotplug is not promised; a device that
the backend adds later is a re-open, not a surprise event.

### ioctls

The names come from `<linux/input.h>`; the provider publishes an ioctl table
with exact sizes and directions.

| ioctl | behaviour |
| --- | --- |
| `EVIOCGVERSION` | the evdev protocol version (`0x010001`) |
| `EVIOCGID` | `bus`, `vendor`, `product`, `version` from the model |
| `EVIOCGNAME`/`EVIOCGPHYS`/`EVIOCGUNIQ` | a stable string per device |
| `EVIOCGBIT(EV_*)` | capability bits: `EV_SYN`, `EV_KEY`, `EV_REL`, `EV_ABS`, ... |
| `EVIOCGKEY`/`EVIOCGLED`/`EVIOCGSW` | current state where the model tracks it |
| `EVIOCGABS`/`EVIOCSABS` | axis ranges for a touch/absolute device |
| `EVIOCGPROP` | properties (e.g. `INPUT_PROP_DIRECT`) |
| `EVIOCGRAB` | exclusive grab; a second grab is `EBUSY`; release on close |

Anything else: `ENOTTY`.  A wrong size or a bad pointer is `EINVAL` before the
provider sees it.

### Events

- `read` returns one `struct input_event` at a time (24 bytes on arm64:
  `timeval` + `type` + `code` + `value`); `O_NONBLOCK` returns `EAGAIN`.
- The model emits complete frames: events followed by `EV_SYN`/`SYN_REPORT`.
  It may coalesce motion; it must not split a report across reads in a way the
  kernel would not.
- Timestamps are `CLOCK_REALTIME` by default; `EVIOCSCLOCKID` may select
  `CLOCK_MONOTONIC`.  The value must be the Android input event's time or a
  monotonic conversion, not `gettimeofday` at delivery.
- Keyboard: `EV_KEY`/`KEY_*`; the model does not do keymaps or compose text.
  That is the client's (`libinput`, X, a compositor) job.
- Mouse: `EV_REL`/`REL_X`,`REL_Y`, `EV_KEY`/`BTN_*`, `REL_WHEEL`.
- Touch: `EV_ABS` with `ABS_MT_SLOT`, `ABS_MT_TRACKING_ID`,
  `ABS_MT_POSITION_X/Y`, `BTN_TOUCH`; the same frame rules as the kernel's
  multitouch protocol.
- `poll` reports readable when a frame is queued; provider death reports
  `POLLHUP` and every later `read` is `EIO`.

### sysfs and udev

This is the open wall.  `evtest` opens `/dev/input/eventN` directly and works
with the model alone.  **libinput needs udev**: it enumerates
`/sys/class/input/eventN` and reads `/run/udev/data`.  v1 promises the direct
path, not libinput.

A later step is a **minimal udev view**: a synthetic `/sys/class/input/*`, a
`/run/udev/data/c13:N` entry, and the `ID_INPUT*` properties libinput reads.
That is a separate deliverable with its own tests; without it, a compositor
cannot use the device.  Do not pretend a device exists in sysfs until that view
is consistent.

## uinput

### The node

| | |
| --- | --- |
| guest path | `/dev/uinput` |
| kind | `char` |
| `st_rdev` | `10:223` (`MISC_MAJOR`, `UINPUT_MINOR`) |
| model | a factory for the prefix's evdev devices |
| access | write; the fd owns one not-yet-created device |

The app cannot open the host's `/dev/uinput` (and should not; that would inject
into Android).  The runtime provides a **prefix-local factory**: the ioctls
configure a device, `write` sends events, and the device appears in the prefix's
evdev set.  Android never sees it.

### ioctls

| ioctl | behaviour |
| --- | --- |
| `UI_SET_EVBIT`/`UI_SET_KEYBIT`/`UI_SET_RELBIT`/`UI_SET_ABSBIT`/`UI_SET_MSCBIT`/`UI_SET_LEDBIT`/`UI_SET_SWBIT` | enable a type/code in the pending device |
| `UI_SET_PROPBIT` | set a property bit |
| `UI_SET_PHYS` | set the physical path string |
| `UI_DEV_SETUP` | name, id, `ff_effects_max` |
| `UI_DEV_CREATE` | publish the device; it becomes an `eventN` |
| `UI_DEV_DESTROY` | remove it; open readers get `POLLHUP` |

After `UI_DEV_CREATE`, `write` accepts `struct input_event` frames.  The factory
validates the type/code against the enabled bits; an unknown code is `EINVAL`.
`SYN_REPORT` ends a frame, as on the kernel.  The created device's `EVIOCG*`
answers describe what was configured (name, id, bits, absinfo).

### Lifetime

- One device per open fd; closing before `UI_DEV_CREATE` discards it.
- Closing after create does not destroy it; `UI_DEV_DESTROY` does.
- The created device lives until destroyed or the prefix ends.
- Its minor is allocated by the provider; it does not collide with the
  boot-time devices.

### Uses

- `ydotool` and automation inside the prefix.
- An accessibility tool that drives a compositor.
- Tests and fault injection.

It is deliberately **not** a way to drive Android: that would be authority, and
is refused.

## Tests

- evdev: `evtest` on each fixed device; a C test reads a keyboard frame, a
  mouse move, and a multitouch frame with a grab; `EVIOCGRAB` contention;
  `O_NONBLOCK`; provider death.
- uinput: `evemu-play` or a small C tool creates a keyboard, emits keys, and a
  reader receives them; `UI_DEV_DESTROY` wakes readers; a bad code is `EINVAL`.
- udev view (when it exists): `libinput debug-events` sees the device.

## Milestones

| | scope |
| --- | --- |
| **I1** | evdev fixed devices, path-only; `evtest` passes |
| **I2** | uinput factory; a created device reads back |
| **I3** | minimal sysfs + udev view; `libinput` |
| **I4** | hotplug and multiple devices, if a client needs them |

## Non-goals

- Injecting input into Android.
- A full udev daemon; only the view libinput reads.
- Keymaps, compose, IME; those are clients.
- Hotplug in v1.

## Open questions

1. **udev view**: exact `/sys/class/input` and `/run/udev/data` fields libinput
   needs on Android 12 and 16; can it be a static view?
2. **Device set**: fixed keyboard/mouse/touch, or host-declared names in
   `boot/dev` options?
3. **Timestamps**: the Android `InputEvent` clock vs the guest's
   `CLOCK_REALTIME`; how to convert without a jump.
4. **Grab**: should `EVIOCGRAB` on the prefix keyboard stop Android input too?
   No -- that would be authority; it only affects the prefix's readers.
5. **uinput minors**: allocate from a fixed range, or let the provider choose
   and report it through sysfs (when I3 exists)?

## Sources

The Linux `input`/evdev UAPI (`input.h`, `input-event-codes.h`) and
`libinput`/`libudev` documentation; `illiliti/libudev-zero` for a daemonless
udev; `evemu`, `evtest`, and `ydotool` for tests.  DroidDeck's input/udev
preloads are the Android-side catalogue
([`../../reference/prior-art.md`](../../reference/prior-art.md), "On Android").
