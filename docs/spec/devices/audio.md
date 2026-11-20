# Audio: a host service, not `/dev/snd`

Status: **proposal** (2026-10-19).  Not in 0.9.0.  Audio is the one "device"
where the smallest honest path is **not** a `/dev` model: ALSA's ioctl surface
is large, and the app already owns the stream through AAudio/OpenSL ES.  v1 is
a host service in the tree plus a backend in the app; a `/dev/snd` model is a
later, separate proposal.

Named workload: `aplay`/`speaker-test`, a desktop notification sound, and a
program that opens ALSA directly.

## Shape

```
guest program
   │  ALSA lib (libasound)
ALSA plugin  libasound_module_pcm_debi.so     (in the tree)
   │  UNIX socket + shared ring + doorbell
app backend (AAudio)  ──> Android audio focus / route / volume
```

- The plugin is a normal ALSA PCM plugin.  Programs that use `libasound` need no
  change; `aplay -D debi file.wav` works.
- The app owns the stream, the audio focus and the device route; the plugin
  only moves frames and reports state.
- The runtime is not in this path.  It does not need a new syscall or a device
  table entry; the socket is an ordinary AF_UNIX path in the tree.

## The plugin

- PCM parameters: `S16_LE`, `S32_LE` and `FLOAT_LE`; 1-2 channels (more if the
  app supports it); 44.1/48 kHz; the app resamples if needed.
- Period/buffer sizes: the plugin advertises a small set that the backend can
  meet; `snd_pcm_hw_params` rejects the rest rather than guessing.
- Data: a shared memory ring (memfd) with a producer/consumer index; the plugin
  writes frames, the app reads.  A doorbell (eventfd/socket) wakes the app.
  One copy; no per-period message.
- State: `prepare`/`start`/`drain`/`pause`/`drop`/`xrun`; `poll`/`snd_pcm_wait`
  semantics; `avail`/`delay` from the ring and a played-frames counter.
- Timestamps: the app reports the frame position; the plugin derives
  `snd_pcm_status` without inventing a clock.
- Errors: backend gone → `-ENODEV` on the next operation; a full/empty ring
  blocks or returns `-EPIPE` according to the ALSA mode.

## The backend

- `AAudioStream` (preferred) or `OpenSL ES` as a fallback.
- Audio focus: request/abandon; pause on loss; duck if asked; restore on gain.
- Route/volume: follow the Android device; do not fake a mixer that the app
  does not have.
- Format conversion and resampling: the app or the plugin, but only once.
- Latency: report the real buffer; do not promise pro-audio latency.

## Mixing and multiple clients

v1 promises **one stream per prefix**.  Two programs that open the plugin at
the same time either share the ring through a small in-tree mixer or the second
open fails with `EBUSY` (decide before implementing).  A real mixer belongs to
PulseAudio/PipeWire, not to this plugin.

PulseAudio and PipeWire clients are not promised in v1.  If a program speaks
only Pulse, a Pulse-to-ALSA bridge (`apulse`, or a pipe sink) can sit in the
tree, but that is a client-side workaround, not this spec.

## What we do not do in v1

- A synthetic `/dev/snd/*` (ALSA's ioctl model is large; the plugin avoids it).
- Kernel-enforced audio isolation; the app's focus is the policy.
- Multi-stream mixing, effects, MIDI.
- Reaching the host's audio devices directly.

## Tests

- `aplay` with a WAV on the default PCM; `speaker-test` for channels.
- A C program using `snd_pcm_*`: parameters, `snd_pcm_wait`, `avail`, xrun,
  drain.
- Focus: a phone call/another app takes focus; the stream pauses and resumes.
- Latency and xrun under load; the ring does not spin.
- Volume/mute follow the Android stream.

## Milestones

| | scope |
| --- | --- |
| **A1** | plugin + socket + ring; `aplay` plays |
| **A2** | focus, pause/resume, volume/mute, route |
| **A3** | xrun/poll/timestamps; one more format |
| **A4** | multiple clients (mixer or `EBUSY`), Pulse bridge if needed |

## Open questions

1. **AAudio vs OpenSL**: AAudio needs API 26+; the app's minSdk is 24.  A
   fallback is required, or bump the floor.
2. **Ring ownership**: the app allocates the memfd or the plugin?  Who resets
   it on a backend restart?
3. **Resampling**: app or plugin?  One side must own it to avoid two.
4. **Multiple clients**: `EBUSY` is honest but breaks a desktop; is a tiny
   in-tree mixer worth it before Pulse?
5. **`/dev/snd`**: only if a named client refuses ALSA plugins; do not build it
   speculatively.

## Sources

Winlator's `android_alsa` bridge
([`../../reference/prior-art.md`](../../reference/prior-art.md), "On Android");
the AAudio/OpenSL ES documentation; the ALSA PCM plugin documentation for
`libasound_module_pcm_*`; PulseAudio/PipeWire only as a client workaround.
