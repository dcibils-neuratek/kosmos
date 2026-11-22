# USB audio, and choosing the sound devices

Sound out of a USB speaker, and a choice of which device plays. Diego, 8
October 2026: "do we support usb audio? i have a usb speaker that i would
like to try in kosmos", then "the mixer app that shows volumes should have
a sound out and in device selector. we need to know which device is
selected", "we are adding usb audio and else so we need to select which
audio device is for input and for output", "also what bitrate, hz and
channels the device has", "16 bit, 24 bit, etc". And 22 November: "I want
to do usb audio so i can test my edifier usb speakers which they work with
audio over usb".

This is the design, written before any of it is built. Mixer as it will
look is `docs/usbaudio.html`; what it stands on is
`docs/usbaudio-architecture.png`. Each step's account is added here as it
lands, as `usb.md` does.

---

## What a person can do

- **Plug in a USB speaker, headset or DAC and hear it named**: a
  notification with its name ("Edifier speakers"), and the device in Mixer
  at once. Nothing to install.
- **Choose which device plays.** Mixer's Output chooser lists every output:
  the machine's own (HD Audio on a PC, virtio-sound in QEMU) and each USB
  one. Choosing moves every program's sound there at once, mid-song,
  without the program knowing.
- **See what each device is running at, and what it can do**: its name, its
  connection ("USB 2.0, Audio Class 2"), its sample size (16, 24 or 32
  bits), its rate (44.1, 48, 96, 192 kHz) and its channels, as it is now
  and as it offers.
- **Choose its format**, from what it offers, where it offers more than one.
  The default is the best it offers that the system mixes without loss
  (below).
- **Have the choice kept.** The output chosen is remembered by name. When
  that speaker is plugged in again, sound goes back to it. When it is
  pulled out, sound goes to the machine's own device with no gap longer
  than the unplugging itself, and a notification says so.
- **Turn it up and down with its own buttons.** Most USB speakers with a
  volume knob or buttons report them as a HID consumer control. Those keys
  move the master, as the keyboard's volume keys do now.
- **Choose which device records**: an Input chooser, beside the Output one,
  with each microphone or line in, its format and a level meter. Recording
  itself (a program asking for a microphone) is the last step here and is
  shaped for, not built first (*Input*, below).

Not now: Bluetooth audio (`bluetooth.md` B8), several outputs at once, an
equaliser (its own roadmap entry), and surround beyond what a device's
channels are mixed down to.

---

## The architecture

```
  Mixer, the Deskbar's volume, notifications                (Lua, the UI kit)
          |  /Devices/audio: audioproto.h, three operations new
  --------------------------------------------------------------------------
  audio: the server                                        (C, the AUDIO band)
     streams    each program's ring, as now
     mix        every stream summed, gain and balance, as now
     outputs    NEW: a list of devices; one chosen; the mix converted to
                its rate, size and channels (pcmconvert.h) and written to it
          |                                   |
          | SYS_SND_WRITE (as now)            | NEW: a ring of periods in a
          |                                   | region, and pulses both ways
  -----------------------------       ---------------------------------------
  the kernel's HAL: HD Audio,          xhci: the USB driver          (C, a driver)
  virtio-sound (as now)                  uac_decode.c  NEW: a device's audio
                                                       descriptors read - pure C,
                                                       held on the Mac
                                         isochronous OUT  NEW: TDs a millisecond
                                                       (or a microframe), filled
                                                       from the ring
                                         feedback     NEW: an asynchronous
                                                       device's rate, read from
                                                       its feedback endpoint
                                         HID          the speaker's buttons, as
                                                       a keyboard's volume keys
```

**Why the audio server is still the one door.** It is the one process that
may make a noise (`audio.c`, `SPAWN_AUDIO`), and per-program volume exists
because every stream passes through its mix. A USB speaker is one more
place for the mix to go, not a second mixer. Programs keep writing to
their rings and never learn which device plays. That is what lets a choice
in Mixer move a song mid-play.

**Why the USB part lives in `xhci.c`, beside the camera.** The controller's
rings, slots and endpoints are the driver's, and an isochronous endpoint is
a slot on the controller's schedule; only the process that owns the
controller can keep it fed. The camera already does this inward (`usb.md`
8b). The class's knowledge, which is most of it, goes into
`uac_decode.c`: descriptors in, a description of the device out, with no
hardware in it and a host test on the Mac, as `uvc_decode.c`,
`midi_decode.c` and `btusb_decode.c` are. A UAC device is a class driver
of the same size as the camera's.

**Data and control, as the rule says.** Periods are a stream the hardware's
clock brings round, so they travel in a region: one ring per USB output,
written by the audio server and read by `xhci`, single producer and single
consumer, indices only (`audioring.h`'s shape, the ring clients already
use). Messages carry only what changes: a device arrived or left, its
formats, the one chosen. **Pulses** (`pulses.md`) carry the two wake-ups:
`xhci` to the audio server, "room in the ring"; and the server to `xhci`,
"a period is in". Neither waits on the other (*Nothing waits on hardware*,
`CLAUDE.md`).

**The clock is the device's.** HD Audio's interrupt paces the server today.
A USB speaker takes exactly what its clock takes:

- **Adaptive or synchronous** (most UAC1 speakers): the host sends the
  nominal rate, 48 samples a millisecond at 48 kHz, or 44 and 45 in turn at
  44.1 kHz. The device follows the host.
- **Asynchronous** (most UAC2 DACs and many speakers): the device has its
  own crystal and says through a feedback endpoint, every few
  milliseconds, how many samples it wants per frame (10.14 fixed point at
  full speed, 16.16 at high speed). `xhci` sizes each TD from that, so the
  ring drains at the device's rate, and the server sees only "room".

Either way the server's loop is the same as now: fill while there is room,
sleep on the receive until woken. The device's drain is the wake-up.

**One mix rate, converted at the door.** Every client writes at the rate
`/Devices/audio` reports, 44.1 kHz now (`HAL_SND_RATE`). Most USB speakers
take 48 kHz, some only 48. Changing every client's rate at a switch would
mean every program reopening its stream mid-song, and none of them is
written to. So the server mixes at one rate and converts at the output:
`pcmconvert.h`, the resampler `sys.pcm` and the Playback Kit already share,
held to its own host test. Sample size and channels convert on the same
pass: 16 to 24 or 32 bits is a shift, stereo to more channels is the two
copied, more to stereo a sum. *Decision 1* asks whether the mix rate should
become 48 kHz for everyone instead.

**What is C and what is Lua.** Everything on the sound path is C: the
server, the conversion, the driver. It is on a deadline and its loops are
over samples. Mixer, the notification and the library are Lua. They list,
choose and show; they never touch a sample.

**What already supplies each piece** (`CLAUDE.md`, *Kits, servers and
drivers supply*):

| piece | supplied by |
|---|---|
| mixing, gain, balance, per-program streams | the audio server, as now |
| rate conversion | `pcmconvert.h`, shared with `sys.pcm` and the Playback Kit |
| the ring between server and driver | `audioring.h`'s shape |
| wake-ups | pulses (`pulses.md`), built in 0.11.227-0.11.230 |
| isochronous transfers | `xhci.c`'s camera path, turned outward |
| a device's descriptors | `uac_decode.c`, new, pure C |
| the speaker's buttons | `usb_decode.c`'s HID report reader, as a keyboard's |
| "plugged in" | `notify` (the notification server), as a stick's |
| the list and the choice | `audioproto.h`, three operations added |
| Mixer's chooser | the UI kit's dropdown and cards |

New: `uac_decode.c`, isochronous OUT and feedback in `xhci.c`, the output
list in `audio.c`, and three operations. Nothing here is a second copy of
anything.

**The protocol** (`audioproto.h`), each a declared shape:

- `AUDIO_OP_DEVICES`: every device, output and input, with an id, its name,
  its connection, the format it runs at, and the formats it offers (sizes,
  rates, channels as bit sets).
- `AUDIO_OP_CHOOSE`: this device for output (or for input), at this format,
  0 for its default.
- `AUDIO_OP_WATCH`: a pulse to the caller when the list changes, so Mixer
  redraws when a speaker is plugged in, without asking twice a second.

And from `xhci` to the server, on an endpoint init hands only to the audio
server: a USB output arrived (its id, formats and ring region), left, or
changed format.

**The busiest path: a period, end to end.** A program writes 256 frames to
its ring and says nothing. The server, woken by `xhci`'s pulse that the USB
ring has room, sums every stream (as now), converts 44.1 to 48 kHz and 16
to 24 bits (`pcmconvert.h`), and writes the period into the USB ring. Then
it pulses `xhci`. `xhci`, on its next millisecond, copies the next
millisecond's samples into the TD the controller is about to need, and
rings the doorbell. The device plays it. Where the time goes: the mix is
the server's cost today; the conversion is one more pass over the period,
a few microseconds at 48 kHz in C with vector types; `xhci`'s copy is 192
bytes a millisecond. The latency is the ring's depth: four periods, about
23 ms, as HD Audio's is now, and *Decision 2* asks whether a USB output
runs shallower.

**Pulled out mid-song.** `xhci` sees the port go (it already does), stops
the endpoint and tells the server. The server marks the output gone and
moves the mix to the machine's own device at once; the periods in the USB
ring are lost, which is the unplugging. The choice is kept by name for when
it comes back.

---

## Input

No device records yet: HD Audio's capture is not driven, and nothing in
Kosmos asks for a microphone. So Input is designed now and built last:

- The Input chooser lists inputs from the same `AUDIO_OP_DEVICES`: a USB
  microphone's or headset's isochronous IN endpoint (the camera's
  direction, already built), and HD Audio's line in when it is driven.
- A level meter beside each, which is the first proof it works.
- A program asks for the microphone by opening a stream that reads
  (`AUDIO_OP_OPEN` with a direction), and only if its header says
  `kosmos: needs microphone`, as the camera's `needs camera` does.

---

## How it is tested

**In QEMU, every step but the speaker itself.** QEMU has a USB speaker,
`-device usb-audio` (UAC1, 48 kHz, 16 bits, stereo, adaptive), and its
sound can go to a file, `-audiodev wav`, which `run_film.py` already uses
to hear a film. So the gate:

- plays a known tone through the USB speaker and reads the WAV QEMU wrote:
  the tone's frequency within a tenth of a hertz (the rate converted
  right), no gap (no period missed), the level as Mixer set it;
- switches output mid-tone, from HD Audio to USB and back, and finds the
  tone continuing in each file with a gap no longer than a period;
- unplugs the speaker mid-tone (QEMU's monitor, `device_del`) and finds the
  sound continuing on HD Audio;
- lists devices through `AUDIO_OP_DEVICES` and holds Mixer's chooser to
  that list.

**On the Mac, the decoder.** `tools/test_uacdecode.c` holds `uac_decode.c`
to real descriptors: QEMU's speaker, a UAC2 DAC's, and the Edifier
speakers' once read. Its control feeds each a descriptor with one field
broken.

**On the M700, the Edifier speakers.** Played, switched, unplugged and
plugged back, with `groove --report`'s counts for underruns, by ear.

---

## Steps

Each is small enough to be checked by itself:

- **U1, the speaker named.** `uac_decode.c` and its host test; `xhci.c`
  calls a configuration with an Audio Streaming interface a speaker, and
  logs what it offers. `lsusb` says "driven: speaker". On the M700, the
  Edifier speakers' descriptors read and kept as the test's fixture. This
  answers every question this document has about them.
  **Built in 0.12.77** (`testing.md` 18.996): `uac_decode.c` and its 63
  checks, QEMU's speaker byte for byte among them; `xhci.c` names it and
  says its setting; `lsusb` lists it "driven: audio". The Edifier
  speakers' descriptors are what is left, when they are plugged into the
  M700.
- **U2, isochronous OUT.** `xhci.c` keeps a speaker's OUT endpoint fed
  with TDs from a buffer of its own, a tone made in the driver, behind
  `opt/kosmos/usbaudio=tone`, as the camera's pattern is. QEMU's WAV holds
  the tone.
- **U3, the ring and the server.** The audio server holds a list of
  outputs, takes a USB one when it arrives, and writes its mix there
  through the ring, converted. Chosen by `opt/kosmos/audio=usb` until U4.
  Groove plays through QEMU's USB speaker.
- **U4, the choice.** `AUDIO_OP_DEVICES`, `_CHOOSE` and `_WATCH`; switching
  mid-stream; unplugging; the choice kept by name in `/Home/Settings`.
- **U5, Mixer.** The Output chooser, each device's format and what it can
  do, as `docs/usbaudio.html` draws it; the notification when one is
  plugged in.
- **U6, asynchronous devices.** The feedback endpoint read and each TD
  sized from it. QEMU's speaker is adaptive, so this is held by a host test
  of the arithmetic and by the Edifier speakers if they are asynchronous.
- **U7, high speed and UAC2.** Microframe intervals, 24 and 32 bits, rates
  to 192 kHz, a format chosen in Mixer.
- **U8, the speaker's buttons.** Its HID consumer controls as the volume
  keys.
- **U9, input.** A USB microphone's stream, the Input chooser's meter, and
  `kosmos: needs microphone`.

U1 to U5 are what plays the Edifier speakers if they are UAC1 and
adaptive; U6 and U7 if they are not. U1 says which.

---

## Decisions - agreed

**Agreed on 22 November 2026, every recommendation taken** (Diego, 22 November: "agreed with your recommendations, go ahead with usb audio"). Each decision below stands as recommended.

1. **The mix rate.** (a) Keep mixing at 44.1 kHz and convert to each
   device's rate at its door *(recommended for now)*. Nothing that plays
   today changes, and the conversion is one tested pass. (b) Mix at 48 kHz
   everywhere, as macOS and most systems do, and convert CDs and MP3s at
   44.1 instead. Fewer conversions on USB, one more on every song, and every
   client's rate changes at once. Worth revisiting when 24-bit and 96 kHz
   playback (U7) arrive.
2. **A USB output's depth.** (a) Four periods, about 23 ms, as HD Audio is
   *(recommended)*: the same latency everywhere, and the same safety under
   load that `testing.md` 18.263 measured. (b) Two, about 12 ms, nearer the
   "before the ear can tell" this system aims at (`CLAUDE.md`), at the risk
   of a gap under load. Measured on the M700 at U3, then chosen.
3. **When a chosen speaker is plugged in again**, (a) sound goes back to it
   by itself *(recommended)*, as macOS does, or (b) it waits for Mixer.
