# DRM/KMS: a study, not a promise

Status: **study** (2026-10-19).  This is not a device model commitment.  It
records what a synthetic `/dev/dri/card0` would need, why it is much larger than
fbdev, and which client would justify it.

Target, if it ever becomes one: a guest compositor (wlroots, Mutter) or SDL2
KMSDRM.  If the only target is a single full-screen app, fbdev
([`fbdev.md`](fbdev.md)) is the smaller answer.

## Why it is large

A DRM client does not just draw; it enumerates and configures a card:

- `DRM_IOCTL_VERSION`, `GET_CAP`, `GET_DRIVER`, `SET_CLIENT_CAP`;
- `MODE_GETRESOURCES`, `GETCONNECTOR`, `GETENCODER`, `GETCRTC`, `GETCRTC2`;
- `CREATE_DUMB`, `MAP_DUMB`, `DESTROY_DUMB`, `PRIME_HANDLE_TO_FD`,
  `PRIME_FD_TO_HANDLE`;
- `ADDFB`/`ADDFB2`, `SETCRTC`, `PAGE_FLIP`, `WAIT_VBLANK`;
- atomic: object properties, `ATOMIC`, `ATOMIC_*`, fences, formats/modifiers;
- events: page-flip completion, vblank, hotplug;
- `mmap` of dumb buffers, offsets, sizes;
- sysfs/udev and a seat (`logind`/`libseat`) for a compositor.

fbdev is one buffer and a pan.  DRM is an object graph and a state machine.

## The walls

- **GBM/EGL**: a compositor's renderer wants GBM on the DRM fd and EGL on top.
  A synthetic card must satisfy GBM (buffer allocation, formats, modifiers) or
  the compositor falls back to software.  `kms_swrast` may work with dumb
  buffers; it is not the default.
- **GPU**: Android's GPU is kgsl/Turnip, not a DRM card.  A DRM node can hand
  out dumb buffers, but real GPU rendering (dmabuf, modifiers) needs a bridge
  that is research, not a table entry.
- **Input/seat**: a compositor needs evdev (`devices/input.md`) and usually
  libinput/udev.  Without the udev view, DRM alone is not enough.
- **Consistency**: libdrm probes a lot; one wrong capability or property and
  the client picks a different path or fails.

## What a minimal model could promise

If a target appears, the first subset is:

- one connector, one encoder, one CRTC, one primary plane;
- a fixed mode and a dumb buffer;
- legacy `SETCRTC` + `PAGE_FLIP` + page-flip events;
- `PRIME` only if the backend can supply a dma-buf;
- no atomic, no planes beyond primary, no hotplug, no fences.

That is enough for SDL2 KMSDRM and *maybe* a software compositor; it is not
enough for a modern compositor.  The model must say which client it is for.

## Prior art

- **VKMS** (kernel virtual KMS): the semantics to imitate, but it is a kernel
  module; we would implement the UAPI in userspace.
- **Mesa `drm-shim`**: a narrow userspace DRM for tests; proof that a subset is
  possible, not a general driver.
- **drm_hwcomposer/minigbm**: Android's own composition path; not accessible to
  an app.
- **QEMU virtio-gpu**: a real device model with a guest kernel; the VM route,
  not the prefix route.

## Decision rule

Do not start DRM until a named client is chosen and measured.  If the client is
a full compositor, also specify the udev view and the renderer fallback first.
If the client is one KMS app, check whether SDL/KMSDRM can use a smaller fbdev
path instead.

## Open questions

1. **Target**: SDL2 KMSDRM, kmscon, or a compositor?  The answer changes the
   subset.
2. **GBM**: software only (`kms_swrast`), or a dmabuf bridge to kgsl?
3. **Atomic**: can the target run on legacy `SETCRTC`, or does it require
   atomic?
4. **Seat**: how does the guest compositor get DRM master without logind?
5. **Effort**: is this weeks or months compared with a nested X/Wayland
   server?  Measure before committing a milestone.

## Sources

The DRM/KMS UAPI (`drm.h`, `drm_mode.h`), GBM and EGL documentation; VKMS as
the kernel model, Mesa `drm-shim` as the narrow userspace proof; the Crostini
and AVF platforms for the VM route
([`../../reference/prior-art.md`](../../reference/prior-art.md), "Other hosts,
devices, and ABI").
