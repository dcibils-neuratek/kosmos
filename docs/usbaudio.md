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
  **Built in 0.12.79, and played on the M60** (`testing.md` 18.998): ten
  beeps of 440 Hz, 480,000 frames, none missed. The M60 is UAC1 and
  synchronous, so U6 is not on its path.
- **U3, the ring and the server.** The audio server holds a list of
  outputs, takes a USB one when it arrives, and writes its mix there
  through the ring, converted. Chosen by `opt/kosmos/audio=usb` until U4.
  Groove plays through QEMU's USB speaker.
  **Built in 0.12.80, and Basket Case played on the M60** (`testing.md`
  18.999): one region from init for the ring, the server's mix converted
  by `pcm_convert_to`, the driver's pulses.
- **U4, the choice.** `AUDIO_OP_DEVICES`, `_CHOOSE` and `_WATCH`; switching
  mid-stream; unplugging; the choice kept by name in `/Home/Settings`.
  As it is being built:
  - **The ring says who is there.** Its header grows from 64 bytes to 128,
    adding the speaker's product string, its vendor and product numbers, and
    the formats its settings offer: their sample sizes as a bit set and up
    to six rates. `xhci` writes these before it sets `present`, as it does
    the format. So the server can list a speaker without a second channel
    from the driver: the region it already has says everything.
  - **`xhci` streams any speaker that is plugged in**, not only under the
    boot word. With nothing written it sends silence and pulses nobody,
    so an unchosen speaker costs the driver a TD's copy a millisecond and
    the server nothing. It pulses the server when frames were taken *and*
    the server is writing, and when a speaker arrives or leaves.
  - **The choice is a name, held by the server.** An empty name, which is
    how the server starts, means the newest output: a speaker while one is
    plugged in, otherwise the machine's own (the mockup's "Plugged in.
    Sound is playing here."). A name means that device whenever it is
    present, and the machine's otherwise. So a chosen speaker pulled out
    and plugged in again gets the sound back by itself (decision 3), and
    choosing the machine's device while a speaker is in keeps it there.
    `opt/kosmos/audio=machine` starts the server with that choice, which is
    the controls' way of keeping HD Audio with a speaker plugged in.
    `opt/kosmos/audio=usb` is no longer needed and goes.
  - **Kept across a restart by the person's side.** The server has no
    namespace and writes no file. `audio.lua`'s `choose` sends the name and
    keeps it with the settings kit (`prefs.lua`, the file `audio` in
    `/Home/Preferences`). The Deskbar sends it again when it starts, as it
    reads its looks then: it is the one program every session has. (This
    said `/Home/Settings`, and "as the Deskbar does its volume". The kit's
    folder is `/Home/Preferences`, and nothing restores the volume yet.)
  - **Watching** is a folder's watch, unchanged in shape (`pulses.md` P4):
    a window asks the window manager for a code and a door, passes the door
    with `AUDIO_OP_WATCH`, and the server pulses every door it holds when
    the list or the one playing changes. `AUDIO_OP_UNWATCH` with the code
    takes it back, and a door whose pulse fails is dropped. The list of
    doors grows as asked; nothing caps it.
  - **Held by** QEMU: a speaker plugged in mid-song with `device_add` (the
    sound moves to it, and QEMU's WAV has the rest of the tone), pulled out
    with `device_del` (the sound is back on HD Audio within a period, and
    the program writing it never knew). Then the machine's device is
    chosen while the speaker is in, and the WAV stops growing. Each check
    has its control.
  **Built in 0.12.82** (`testing.md` 18.1001), as written above but for
  two things learned building it. The machine's own device is named
  "Built-in", since the kernel says no codec's name to a program yet. And
  the WAV never stops growing: an unchosen speaker is fed silence, so the
  check looks for the tone in what was written after the choice, not for
  the file's size.
- **U5, Mixer.** The Output chooser, each device's format and what it can
  do, as `docs/usbaudio.html` draws it; the notification when one is
  plugged in.
  **Built in 0.12.83** (`testing.md` 18.1002). Mixer's Output card: a
  dropdown of every output, showing the one playing; choosing one is
  `audio.choose`. Under it, three tiles - sample size, rate, channels -
  each with what else the device offers, and "Mixed at 44.1 kHz,
  converted to 48 kHz here" when the device's rate is not the mix's. The
  window is told of a change by the server's pulse, not on its tick, through
  `window:watch_code`, which the UI kit now has for anything that pulses
  (`watch_folder` was the only door before, and JetSki had written its own).
  The Deskbar posts the notification, because it runs in every session and
  Mixer need not be open: "QEMU USB Audio - Plugged in. Sound is playing
  here.", and "... unplugged - Sound is playing on Built-in." **Left from
  the drawing**: the tiles cannot be chosen yet (that is U7); the Input card
  comes at U9; the dropdown lists names, not the drawing's groups ("This
  machine", "USB") with a line of facts under each.
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

- **U10, hi-res** (asked 22 November, after the M60 played: "so we can
  play flac audio file sin full resolution", "in bitrate and all"; for
  Diego to agree, with `docs/player.md`'s decisions 5 to 7, which are its
  own). **AGREED on 22 November, as recommended** (Diego: "agreed with your recommendations, go ahead"): a
  windowed-sinc resampler, the software volume with the badge honest, and
  other sounds shared and said. The M60's setting 1.2 is 24 bits at 48 or 96 kHz, so a 96/24 FLAC
  can reach it unchanged. What it takes, all in the audio server and its
  protocol, the ring and the driver as they are:
  1. `AUDIO_OP_OPEN` with a format - rate, 16, 24 or 32-bit integer,
     channels; 0 is today's 44.1/16.
  2. The mix in 32 bits at the output's rate.
  3. The output's setting and rate follow the highest-resolution stream
     playing, within what the device offers (the driver switches the
     setting: drain, SET_INTERFACE, SET_CUR of the rate, start - timed
     and said); the others are converted into it.
  4. Unity is unity: at full gain and master a stream's samples pass
     untouched; narrowing is exact when the low bits are nought and
     dithered when not.
  5. `exact` per stream with its reason (rate, volume, mixed, narrowed)
     in `AUDIO_OP_STREAMS`, for the player's badge.
  6. `pcm_convert_to` past 16 bits, and the resampler question -
     windowed sinc or linear (player decision 5).
  **Held by** QEMU's speaker at 48/16, the only format QEMU's has: a
  FLAC of noise at 48/16 found bit for bit in QEMU's WAV, broken by a
  volume of 99%, a beep over it, or a 44.1 kHz file - each also said as
  "converted" with its reason. 96/24 is then proved on the M60 by ear
  and by the driver's counts.
  The ring holds 64 ms of 96/24 at nine pages, so it stays.

  **How it is built, in steps** (written 22 November, before the code).
  Every program that plays today - Doom, Quake, Quake III, the SNES, the
  Playback Kit, the Synth Kit, `sys.pcm` - opens a ring of 44.1 kHz 16-bit
  stereo periods. **That ring and that open stay exactly as they are**, as
  format 0. Hi-res is a second format beside it, so nothing that plays has
  to change, or be built again, for any step but U10c.
  - **U10a, the converter past 16 bits.** `pcm_convert_wide` in
    `pcmconvert.h`: 16, 24 or 32-bit integer in, 32-bit frames out at any
    rate. At the same rate it copies, with no interpolation, so a sample is
    itself. Across rates it uses a windowed-sinc filter (decision 5).
    `pcm_convert` stays linear for `sys.pcm` until something asks for
    better. Held by `test_pcmconvert`: bit-exact at the same rate for each
    size, and a sine resampled 44.1 to 48 and 48 to 96 with its error
    measured against the linear one.
  - **U10b, the mix in 32 bits.** The sum in 64 bits, the mix in 32. The
    machine's device takes it narrowed to 16: exact when the low bits are
    nought, which every format-0 stream's are, and dithered when not. A USB
    speaker takes it through `pcm_convert_wide` at its own size. For
    everything that plays today nothing changes, and `x86-usb-2`'s tone
    checks and `x86-sound` say so.
    **U10a and U10b built in 0.12.84** (`testing.md` 18.1003).
  - **U10c, `AUDIO_OP_OPEN` with a format** - rate, 16, 24 or 32 bits,
    channels - in `audio_request`. The ring's periods are that format's,
    the same length of time as the mix's. The server converts each such
    stream into the mix. The protocol changes here, so installed
    applications are built again (P5 brings them).
    **Built in 0.12.89** (`testing.md` 18.1010): `rate`, `bits` and
    `channels` at the end of `audio_request`, all nought for format 0;
    `audio_period_frames` in `audioproto.h` is the one sum both sides
    make a ring's period from, and the server holds the ring to it
    (`AUDIO_ERR_FORMAT` otherwise). A stream in the mix's own format is
    mixed sample for sample as before; any other goes through its own
    carry and sinc (`pcm_convert_wide`) into the 32-bit mix, a period at
    a time, and its position comes back in its own frames.
    `audio.open(name, periods, { rate, bits, channels })` and
    `audio_stream_open_format` are the two doors.
  - **U10d, the output follows the best stream.** The mix runs at the
    output's rate. A USB speaker's setting and rate follow the
    highest-resolution stream playing, within what it offers, and the
    driver switches them (drain, SET_INTERFACE, SET_CUR of the rate,
    start), timed and said.
    **Built in 0.12.91** (`testing.md` 18.1012). The server asks and the
    driver decides: `want_rate` and `want_bits` in the ring's header
    (`usbaudioring.h`, grown to 192 bytes) are the highest rate and the
    most bits of the streams open, format 0 counting as 44.1/16, and left
    as they were when nothing is open, so silence does not switch a
    speaker back and forth. `xhci` holds them to the speaker's settings
    with `uac_pick_for` - two channels, then no bits short of the stream's
    (widening is exact, narrowing is not), then the stream's rate, then the
    fewest bits past it - and where that is not what plays it says
    `switching`, waits for `played` to reach `write` (6 ms at least, 200 at
    most), and switches in `camera_stop`'s order and `speaker_start`'s,
    which is now `speaker_open` for both. The new format and generation
    come with `switching` cleared last; meanwhile the server mixes nothing
    and sends nothing to the machine's device. **The mix runs at the
    speaker's rate** once it plays one: a period stays as long as the
    machine's, so every ring still lands on it, and each stream is tuned
    again - a format-0 stream into a 48 kHz mix is converted from then on,
    a 48/16 one no longer is, and its position is kept in the new frames.
    A speaker plugged in while a stream plays starts at what it asked for.
    QEMU's speaker has one setting, so `opt/kosmos/usbaudio=switch` makes
    the driver switch to the same one, which is every step but the format;
    the format itself is the M60's to prove, 1.1 to 1.2.
  - **U10e, `exact`.** Per stream in `AUDIO_OP_STREAMS`, with its reason
    (rate, volume, mixed, narrowed), for the player's badge.
    **Built in 0.12.92** (`testing.md` 18.1013): each stream's entry
    carries its format and `inexact`, `AUDIO_INEXACT_RATE`, `_VOLUME`,
    `_MIXED` and `_NARROWED` (`audioproto.h`), nought for bit for bit, and
    `audio.streams()` gives `exact` and `why`. The output is the speaker
    while one plays, the machine's 16 bits otherwise; MIXED means another
    stream was heard - samples, a nonzero peak, a gain - in the last
    period.
  - **U10f, the proof.** QEMU's speaker at 48/16: a FLAC of noise at 48/16
    found bit for bit in QEMU's WAV, and broken, each said, by a volume of
    99%, a beep over it, or a 44.1 kHz file. Then 96/24 on the M60.
    **QEMU's half built in 0.12.93** (`testing.md` 18.1014): noise from a
    known seed, written at 48/16 by a program rather than decoded from a
    FLAC - the FLAC through the Playback Kit is the player's P6, which needs
    the kit to open its stream at the source's format (P3) - found bit for
    bit in QEMU's WAV, 32,768 frames, with `exact` said; and not found, with
    `volume`, `mixed` and `rate` said, at a gain of 253, beside a tone and
    at 44.1 kHz. QEMU's speaker calls 240 of 255 nought decibels and scales
    by it, so `opt/kosmos/usbaudio=full` sets its own volume at its top
    (GET_MAX, SET_CUR on the feature unit), the one level that passes
    samples untouched. A real speaker's own volume is left alone. **For
    Diego**: whether Kosmos should set the M60's own volume to 0 dB, its
    unity, when it opens it - bit for bit at the speaker needs it, and it
    would make it as loud as the server's master allows. The M60 at 96/24
    is the other half, read over Telnet.

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
