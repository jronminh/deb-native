# Reference device: fbdev

Status: **proposal** (2026-10-19).  The first model for the
[device table](../devices.md), chosen because it is small and exercises every
layer: node, fd tag, ioctl table, mmap, a provider, and a host backend.

Target clients: `fbi`, Qt's `linuxfb` platform plugin, and a simple mmap test.
It is a **data** device: it can change what a program sees, never what it may do.

## The node

| | |
| --- | --- |
| guest path | `/dev/fb0` |
| kind | `char` |
| strategy | `broker` |
| `st_rdev` | `29:0` (`FB_MAJOR`, minor 0) |
| mode | from `boot/dev` (default `0660`) |
| model | one fixed screen, one buffer, no hotplug |

A prefix may add `/dev/fb1` with another instance and minor, but v1 promises
only one.  The node exists only if `boot/dev` declares it; without a provider
`open` returns `ENODEV`.

## The screen

- 32 bits per pixel, `FB_VISUAL_TRUECOLOR`, little-endian `XRGB8888`
  (`red.offset=16`, `green=8`, `blue=0`, `transp.offset=24`).
- `line_length = xres_virtual * 4`.
- `yres_virtual >= yres`; the visible window is `(xoffset, yoffset)`, and
  `xoffset` is always 0 (`xpanstep = 0`, `ypanstep = 1`).
- The mode is chosen by the provider at `OPEN` (from the backend's surface or a
  fixed boot mode).  `FBIOPUT_VSCREENINFO` may change only `*_virtual`,
  `yoffset` (and `xoffset`, which must stay 0); changing `xres`, `yres` or
  `bits_per_pixel` returns `EINVAL` in v1.

## ioctls

The names come from `<linux/fb.h>`; the provider's ioctl table lists the exact
numbers, sizes and directions.  v1 promises:

| ioctl | behaviour |
| --- | --- |
| `FBIOGET_VSCREENINFO` | current variable screen info; clock, margin and sync fields are zero |
| `FBIOGET_FSCREENINFO` | `id="debi-fb"`, `smem_len=line_length*yres_virtual`, `type=FB_TYPE_PACKED_PIXELS`, `visual=FB_VISUAL_TRUECOLOR`, `line_length`, `ypanstep=1`, `xpanstep=0`, `ywrapstep=0`, `accel=0`, `capabilities=0` |
| `FBIOPUT_VSCREENINFO` | accepts a virtual size/offset within the allocation; `EINVAL` otherwise |
| `FBIOPAN_DISPLAY` | clamps to the virtual area; asks the backend to present the new window |
| `FBIOBLANK` | `FB_BLANK_UNBLANK`/`FB_BLANK_NORMAL` resume; `POWERDOWN` pauses the backend; other values `EINVAL` |
| `FBIO_WAITFORVSYNC` | best effort in v1: return 0 (no fabricated timestamp) |
| `FBIOGETCMAP`/`FBIOPUTCMAP` | `EINVAL` (truecolor has no palette) |

Anything else: `ENOTTY`.  A malformed argument (wrong size, bad pointer,
out-of-range) is `EINVAL`; it must never reach the provider.

`smem_start` is zero and `mmio_start/mmio_len` are zero: there is no physical
memory to expose.  Programs that insist on a nonzero `smem_start` are outside
the promise.

## mmap

- `length <= line_length * yres_virtual`, `offset == 0`, `MAP_SHARED`,
  `PROT_READ|PROT_WRITE`.
- The provider supplies a `memfd` of exactly `line_length * yres_virtual`
  bytes; the runtime maps it with the requested protection.
- `MAP_PRIVATE` returns `EINVAL` (the screen is shared).
- Writes are visible to the backend after `FBIOPAN_DISPLAY` (or on the
  provider's pace); v1 does not promise any other coherence point.
- `munmap` and remap follow normal VM semantics; the instance is refcounted.

## read/write

`read` and `write` return `EINVAL` in v1.  Clients draw through the mapping;
supporting a file-like write path is not a promise (and would need a second
copy).

## The provider

```
runtime -> provider                provider -> runtime / backend
OPEN   {path=/dev/fb0, flags}      INSTANCE {xres, yres, xres_virtual, yres_virtual, stride}
MMAP   {instance}                  MAP {memfd, offset=0, length}
IOCTL  {GET/PUT_VSCREENINFO, PAN,
        BLANK, ...}                bytes
CLOSE  {instance}                  released
                                   PRESENT {xoffset, yoffset, damage, seq}  (backend)
                                   BLANK {state}                            (backend)
```

- `PRESENT` is asynchronous from the guest's point of view; `FBIOPAN_DISPLAY`
  returns once the pan is accepted, not when the panel shows it.
- `damage` may be `FULL` in v1; a model may refine it later.
- If the backend is slow, the provider may coalesce pans: the guest's last
  `yoffset` wins.  There is no vblank queue in v1.

## The backend

- App: `SurfaceView` + EGL; upload the memfd (or import a dma-buf later),
  draw, and honour the visible window (`yoffset`, `yres`).
- Termux/ADB: a file viewer or a no-op backend; the model still works.
- The backend never sees `/dev`; it sees frames.  This keeps the runtime
  host-agnostic.

## Consistency and failure

- `stat` says `S_IFCHR`, `29:0`, size 0; `open` of a declared node with no
  provider says `ENODEV`; an undeclared node is absent (`ENOENT`).
- No `/sys/class/graphics/fb0` in v1.  `fbi -d /dev/fb0` and Qt `linuxfb`
  work by path; enumerators do not see the device.
- Provider death: existing fds return `EIO`, `poll` reports `POLLHUP`, the
  mapping may still be readable but is no longer presented.  New opens fail
  until a provider appears.
- No hotplug: the mode is fixed for the life of the instance.

## Tests

- `tests/dev/` conformance (see [`../devices.md`](../devices.md), "Conformance")
  with the fbdev provider as the first real model.
- `fbi` (a PNG on the screen), Qt `linuxfb` (a window), and an mmap test that
  fills the buffer and pans.
- Bounds: `length` too large, nonzero offset, `MAP_PRIVATE`, bad `PUT_VAR`.
- Pan clamp; blank/unblank; provider death; undeclared node; `ENOTTY` on an
  unknown ioctl.

## Non-goals

- Mode setting, multiple framebuffers, 8/16 bpp, palette, cursor plane,
  hardware acceleration, vblank queue, hotplug.
- Input: fbdev is output only; keyboard/pointer are a separate evdev model.

## Open questions

1. Who chooses the initial mode -- the backend (surface size), `boot/dev`, or a
   fixed 1280x720 until a client asks?
2. Is a full-frame `PRESENT` per pan enough for the first client, or does the
   backend need damage from the start?
3. `smem_start = 0`: accept the rare client that rejects it, or fake a nonzero
   physical address?
4. Does `FBIOPUT_VSCREENINFO` need to resize at all in v1, or is a fixed mode
   simpler and safer?

## References

[`../devices.md`](../devices.md), `<linux/fb.h>`, `fbi`, Qt `linuxfb`.

## Sources

`<linux/fb.h>` and the `fbi`/Qt `linuxfb` clients; `xspager/fuse_fb` (CUSE) as
the userspace-framebuffer precedent
([`../../reference/prior-art.md`](../../reference/prior-art.md), "Other hosts,
devices, and ABI").  The node and ioctl table are the promise; the provider is
the first model.
