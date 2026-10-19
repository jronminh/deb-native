# The custom-app host: what an app provides a prefix

The runtime is the **core**, and it stays Android-agnostic
([`architecture.md`](../architecture.md), "The core and the variants"):
`dn-tiny`, the shim and the law map a tree onto the phone's kernel and know
nothing of the app around it. But Android is not only a kernel. Its framework --
activities, notifications, display, input, `system_server` -- lives in userland
behind an app's identity, and no syscall in the tree reaches it. So a prefix
runs inside a **host** (the app; also the *container*), and the host provides
what the kernel does not.

This is the contract between the two.  Status: proposal.  The custom app is one
of **three hosts** -- Termux and the app-less **ADB** host (a prefix under
`/data/local/tmp`, driven from a PC) are the others; the overview is
[`hosts.md`](hosts.md), and the ADB host is [`adb-host.md`](adb-host.md).

## Contents

- [What the host provides](#what-the-host-provides)
- [What the host must not do](#what-the-host-must-not-do)
- [The bounds](#the-bounds)
- [Prior art](#prior-art)

## What the host provides

- **The session.** The host extracts the prefix into its files directory and
  runs `ROOT/boot/boot.sh` (or `dn-tiny -t ROOT -- ...` directly), one prefix
  per session, with a foreground service for long work. A prefix names no
  absolute path of its own and works wherever it is put
  ([`building.md`](../building.md), "Moving a prefix"). Choosing among several
  -- the list, a default, a safe boot, a banner -- is the [boot
  manager](boot-manager.md).
- **The boot inputs.** Before boot the host may write `boot/binds` (the host
  directories to bind, and their flags) and `boot/env` (one `NAME` a line keeps
  the host's value, `NAME=VALUE` sets it) -- the data the runtime reads, never
  code in it; `boot/version` says what the prefix is. These are the only knobs;
  the runtime has no other hook for the host.
- **The Android bridge.** The framework calls -- start an activity, raise a
  notification, read the clipboard, a sensor, an intent -- are the host's,
  because it holds the Android identity and the permissions. The host gives the
  tree a **channel**: a helper in the prefix (`/usr/local/bin/dn-host`) or a
  unix socket it listens on, and the tree's programs call it. The runtime only
  keeps the channel reachable: an AF_UNIX path is mapped like any other
  ([`../architecture.md`](../architecture.md), "The law"), a bind exposes the
  socket's directory, and, for a host node, `/dev` and `ioctl` are untouched.
  No Android API is faked in the tracer.  A synthetic device is the deliberate
  exception: that fd is the runtime's, not the host node's
  ([`devices.md`](devices.md)).
- **Display and input.** There is no DRM master for an app: the display is
  SurfaceFlinger's, reached with buffers over binder. So the host is the
  compositor -- a Wayland compositor on a `SurfaceView`, clients handing
  dma-bufs (`zwp_linux_dmabuf_v1`), X through Xwayland, one blit a frame, and
  the input translated from `InputEvent` to Wayland. The GPU is the kernel's:
  render is Turnip on `/dev/kgsl-3d0` ([`../architecture.md`](../architecture.md),
  "Inherited from Android"; [`../../TODO.md`](../../TODO.md), "Graphics").

  The compositor is one backend of the [device table](devices.md), not the
  only one.  There, a prefix declares a synthetic `/dev` node; a provider owns
  the device model and the backend owns the physical side.  The first planned
  model is [fbdev](devices/fbdev.md) (a flat framebuffer, software), which keeps
  the display path host-agnostic and gives the compositor a later step instead
  of a prerequisite.
- **Audio.** Android's audio is the framework's (AAudio/OpenSL ES); a Debian
  program reaches ALSA or PulseAudio. The host bridges it the way it bridges
  display: an ALSA or Pulse plugin in the tree talking to the app over a socket,
  the app owning the stream and the audio focus (Winlator's `android_alsa` is a
  worked example). Same shape, same "the runtime only keeps the channel
  reachable".
- **The services.** A package's `postinst` does not start a daemon in the
  prefix (`init.sh` writes `policy-rc.d` to stop that); a service a program
  needs is the host's to keep -- from a `boot/services` file, or left to the
  program. There is no PID 1 for the tree and none is planned.
- **The runtime.** The host ships `dn-tiny` and `dn-lane.so` and updates them;
  an update of the runtime is then an app update, or a copy into the prefix.

## What the host must not do

- **Do not intercept syscalls or paths.** That is the core's job, and the trap a
  PRoot-shaped container falls into: a second path layer in the app pays the
  same cost twice and hides the one law. The host runs `dn-tiny` and stays out
  of the way -- which is also why the near-native speed is real.
- **Do not fake Android identity.** The tree's root is fake; the app's SELinux
  domain and UID are real, and only the host's own permissions reach the
  framework. A `setuid` inside the tree cannot gain an Android capability.

## The bounds

- **targetSdk 28**, while the app's own `execve` of a file in its data
  directory is required ([`../reference/prior-art.md`](../reference/prior-art.md),
  "Exec on Android: W^X and the linker"). A later target ships the runtime as
  the APK's native libraries and execs through a trampoline -- believed, not
  checked (TODO, "A prefix in any app").
- **Permissions:** the host exposes only what it holds and the platform allows;
  the framework checks the app, not the tree.

## Prior art

DroidDeck is the closest host: a Wayland compositor on a `SurfaceView`, and its
KGSL preloads (`tools/linuxfs/preload/`). Winlator is the fullest instance of the
contract -- its `GPUImage` (an `AHardwareBuffer`, an EGL image, and a dma-buf fd
handed to the app) is the buffer path to a Surface, and it runs the whole
Android glue -- though it also translates the CPU and the graphics API, which a
native arm64 tree does not. dn-shell is the reference container.
The model behind it all is [`pelya/debian-noroot`](https://github.com/pelya/debian-noroot):
a real Debian tree with no root and no `chroot` -- what this project keeps --
run with `fakeroot-tcp` and a set of preloads where deb-native puts the shim and
the tracer ([`../reference/prior-art.md`](../reference/prior-art.md), "On
Android"). That model is the right one; its tooling (PRoot, `fakeroot-tcp`,
2012-era preloads) is what this repo replaces.
