# Video and sound in JetSki: YouTube's player on Kosmos

Written 14 November 2026 and **agreed the same day** - all four decisions
below as recommended (*Decided*, at the end). Diego, before it: "YouTube is my most
used website so it must work perfect", "Watching videos in youtube as is a
top Priority", "So we need to have all decoders needed available"
(`roadmap.md`, *YouTube first, and every decoder*).

## What a person can do with it (the feature set)

- **Watch a YouTube video** in JetSki: picture and sound together, play,
  pause, seek, the quality menu, full screen in the window, at 1080p.
- **Any page's `<video>` and `<audio>`**: Apple's tiles, CNN's clips, a
  podcast - a file given by address, played as it downloads.
- **Media Source Extensions** (MSE), which is how YouTube, Netflix and most
  video sites play: the page's script fetches pieces of the film itself and
  hands them to the browser, choosing the quality as the network allows.
- **The codecs the web uses**, decided by what the page asks
  (`MediaSource.isTypeSupported`): **AV1** and **VP9** with **Opus** in
  WebM, **H.264** with **AAC** in MP4.
- **Sound that does not stutter** on a busy machine - the Playback Kit's
  thread in the audio band, which Music and Video stand on too.

What it does not do, and why: no DRM (Encrypted Media Extensions - Netflix
and Disney+ need Google's Widevine, licensed only to browser makers; YouTube
does not need it for ordinary videos), no WebRTC (calls are another step),
no hardware decoding yet (`docs/m700-gpu.md` is that question).

## Where WebKit leaves off

WebCore draws the controls, runs MSE's rules - buffered ranges, appends,
removals, the timeline - and asks a **media player** for everything that
touches a codec. Every port brings its own player: Apple's is AVFoundation,
GTK's and WPE's are GStreamer. Kosmos has neither: GStreamer stands on GLib
and GObject, which the port left out on purpose (`docs/w6.md`, the generic
run loop), and AVFoundation is Apple's. WebKit also carries a **mock** MSE
player for its own tests (`platform/mock/mediasource/`), which is the
shape of the smallest real one: a `MediaPlayerPrivate`, a
`MediaSourcePrivate` and a `SourceBufferPrivate`.

## The architecture

```
 YouTube's script (JavaScriptCore)
   │  appendBuffer(WebM bytes)          <video> controls, timeline
   ▼                                          │
 WebCore: MediaSource, SourceBuffer ──────────┤      (WebKit, unchanged)
   │                                          │
 ┌─▼──────────────────────────────────────────▼───────────────────┐
 │ the Kosmos player in WebCore (new, C++, the port's)            │
 │   MediaPlayerPrivateKosmos  SourceBufferPrivateKosmos          │
 │   - samples in, frames out; the clock is the sound's           │
 └──┬──────────────────────────┬──────────────────────┬───────────┘
    │ bytes                    │ compressed samples   │ compressed sound
 ┌──▼──────────────┐   ┌───────▼──────────┐   ┌───────▼──────────────┐
 │ Media Kit: demux│   │ Media Kit: video │   │ Playback Kit (exists)│
 │ WebM, MP4, fMP4 │   │ AV1 (dav1d),     │   │ + Opus; its thread   │
 │ (new kit, C)    │   │ VP9, H.264       │   │ in the audio band    │
 └─────────────────┘   │ (FFmpeg)         │   └──────────┬───────────┘
                       └───────┬──────────┘              │ periods (ring)
                               │ frames (a region)       ▼
                         Skia paints the frame      audio server
                         into the page's layer
```

**Does another application want this?** All of it but the WebCore classes.
The Video app plays MP4 today through `mp4.lua` and the H.264 Kit; Music
plays AAC and MP3; Cafesa3D could put a film on a surface; a video editor
is exactly "demux, decode, show". So demuxing and decoding are **one new
kit, the Media Kit** (`/Kosmos/Kits/media`, C), with one door, and JetSki is
its first user and Video its second. The WebCore classes are only the
adapter between WebKit's interfaces and that kit.

**Does something already supply it?**

| Piece | Supplied by | New? |
|---|---|---|
| AV1 | dav1d, built and tested in W5f, one copy with AVIF's | no |
| H.264, AAC | FFmpeg's decoders, vendored since 4e | no |
| VP9, Opus | FFmpeg's decoders, the same FFmpeg | a rebuild |
| Demuxing WebM and MP4 | FFmpeg's `libavformat` (matroska, mov) | yes, from the same FFmpeg |
| Sound on a thread in the audio band | the Playback Kit | Opus added |
| The clock a picture follows | the Playback Kit's position | no |
| A frame onto the page | Skia, from a region | no |
| MSE's rules | WebCore | no |

`mp4.lua` stays for now and is Video's to retire when it moves to the
Media Kit; two MP4 readers is a defect to remove, and this design says
when (step M6).

**What travels where.** A film is a stream, so its bytes never travel as
messages (`CLAUDE.md`, *Control by message, data by shared memory*): the
page's appends are in WebCore's own memory, decoded frames are written into
a ring of frame buffers in a region the painter reads, and the sound goes
through the Playback Kit's two rings as Music's does. All of it is in one
process today; W7's split into a web process and a GPU process moves the
frame ring across a region, which is why it is a ring now.

**What is C and what is not.** Everything here is C or C++: it is all loops
over bytes and samples, and the sound is on a deadline. No Lua.

**The busiest path.** 1080p at 30 frames a second: a frame decoded in under
33 ms, its YUV turned into the page's pixels (libyuv-shaped SIMD, or
dav1d's own output into Skia's YUV path), and 2,048 KB written a frame. On
the M700's four cores dav1d is expected to decode 1080p AV1 in real time with
its AVX2 assembly - expected, not measured here: M1 measures it; FFmpeg's VP9 is built here without assembly (`--disable-asm`,
`tools/ffmpeg_vendor.py`), which is several times slower and is decision 2.

## Decisions for Diego

1. **The player: Kosmos's own** (recommended) - the three classes above on
   the Media Kit, an estimated two thousand lines of port code. *Or GStreamer*,
   which WebKit's GTK and WPE ports use and which works: it brings GLib,
   GObject and a plugin system - dozens of libraries - into Kosmos, against the
   port's choice of no GLib, and a second way of doing everything the
   Media Kit does for Video and Music.

2. **FFmpeg built whole, with its assembly** (recommended), as W5 builds
   dav1d: `libavformat` and `libavcodec` from its own build system for
   x86-64 (nasm) and AArch64, the decoders and demuxers named, the vendored
   closure in `runtime/upstream/ffmpeg/` replaced by it - one FFmpeg. VP9
   and H.264 at 1080p need the assembly. *Or extend the vendored closure*
   with VP9, Opus and the two demuxers, without assembly: smaller, and VP9
   probably too slow for 1080p.

3. **What JetSki tells YouTube it plays: AV1 first** (recommended), then
   VP9, then H.264, each with Opus or AAC. YouTube serves AV1 to a browser
   that says it can, and dav1d is the fastest decoder here. *Or H.264
   first*, the most common and the one with the most hardware behind it -
   for the M700's GPU later.

4. **The order.** M1 the Media Kit (demux and decode, held to FFmpeg's own
   tools on this Mac, file by file, as W5's libraries are); M2 `<video src>`
   in WebCore (Apple's tiles, WebKit's `media/` LayoutTests); M3 MSE
   (WebKit's `media/media-source/` LayoutTests with their own WebM and MP4
   files); M4 the Playback Kit's Opus and the clock; M5 YouTube itself,
   which needs W7's network loader to run live; M6 Video on the Media Kit
   and `mp4.lua` gone. Until W7 YouTube can only be shown with a recorded
   film served from this Mac, which M3's tests already are in shape.

## Decided

Diego, 14 November 2026, each as recommended: **Kosmos's own player** on
the Media Kit, not GStreamer; **FFmpeg built whole with its assembly**,
one FFmpeg replacing the vendored closure; **AV1 first** to YouTube, then
VP9, then H.264; and **the order M1 to M6** as written.

## Built

**M1 begun, 14 November 2026** (`testing.md` 18.804). FFmpeg 9.0.2 is
built whole by `tools/w5_build.py` - its own `configure`, nasm's assembly
on x86-64, NEON on AArch64, threads, the decoders and demuxers above with
the PCM ones, and `libdav1d` as the AV1 decoder, so AV1 has one decoder
here and it is AVIF's. The kit is `user/kits/media/`, with `media.h` as
its door: a container read from the caller's `struct media_io`, packets
and frames timed in microseconds, pictures as planes and sound as
interleaved float. `x86-mediakit` holds it to this Mac line for line and
to Homebrew's `ffmpeg -f framemd5` picture for picture.

What M1 still owes is in `roadmap.md`: decoding speeds on real cores.
The vendored closure leaves with M6, as Video's Lua door arrives.
Video is the closure's one user, and the whole FFmpeg stands on the POSIX
layer, which belongs in an image of its own rather than the system's.


## M2, as it is being built: `<video src>` in WebCore

Diego, 14 November 2026, after Google's search: "I want youtube working
next" - so the media steps come before the JIT. M2 is the first of them:
a `<video>` with a file's address plays its picture in a page.

- **`ENABLE_VIDEO` on**, in the port's options; `ENABLE_MEDIA_SOURCE`
  stays off until M3. Each is a WebKit option, so each is a whole rebuild
  of WebKit: they are turned on once, together with what they need.
- **`MediaPlayerPrivateKosmos`**, the port's engine, in
  `runtime/ports/webkit/Source/WebCore/platform/graphics/kosmos/`,
  registered in `MediaPlayer.cpp`'s list by a patch as GStreamer's is for
  GTK.
- **Bytes through the page's own loader** - WebCore's
  `PlatformMediaResourceLoader`, which is the network process, cookies and
  all - into memory as they arrive. The Media Kit's `struct media_io` reads
  that; a read past what has arrived waits on the decode thread, never on
  the page's.
- **A decode thread** per player: `media_open`, the picture stream's
  decoder, each frame turned into the page's pixels by **`gfx_i420`**, the
  gfx kit's SIMD conversion with BT.601 and BT.709 (one door: the Video
  app's films go through the same function), into a ring of three frames.
- **The clock** is the counter from `play()` until M4 puts the Playback
  Kit's position under it; a frame is shown when its time has come, and
  one that is late is dropped rather than shown late.
- **`paint()`** draws the newest due frame as a Skia image into the
  element's rectangle - in software, as everything on a Kosmos page is
  since patch 0025.
- **What it says it plays** (`canPlayType`, `isTypeSupported` later): what
  the Media Kit decodes - WebM with VP8, VP9, AV1, Opus and Vorbis; MP4
  with H.264, AV1, AAC and Opus; MP3, FLAC, Ogg and WAV.
- **Test, in `x86-jetski`**: a film made at the test's start by this Mac's
  FFmpeg from its own test pattern (no site's bytes), served beside the
  page; the page plays it, and the check is the pattern's colours in the
  window and `currentTime` moving, which the page reports to the server.
  The control is the film served as a 404.

**M2 built, 14 November 2026** (0.11.419, `testing.md` 18.814): a
`<video>` with a file's address plays its picture in JetSki, VP9 in WebM
held by `x86-jetski`. What it does not do yet: sound (M4); 10-bit and NV12
pictures, which it skips; a film's own colour matrix, which the Media Kit
does not report yet (the height decides); and seeking waits for the
decoder rather than answering when the picture at the new time is ready.
Next, M3: Media Source Extensions, which is how YouTube plays.


## M3, as it is being built: Media Source Extensions

YouTube does not give a `<video>` an address. Its player fetches a film
in pieces - picture and sound apart, each in short segments, at a quality
it chooses from the connection - and hands each piece to the browser with
`SourceBuffer.appendBuffer()`. That is MSE, and without it YouTube shows
its page and no film.

- **WebCore keeps the rules.** `SourceBufferPrivate` - the timestamp
  offset, the append window, the buffered ranges, eviction, enqueuing from
  the key picture before a seek - is two thousand lines in WebCore's base
  class, and a port only parses appends into samples and decodes what is
  enqueued. WebCore's own mock (`platform/mock/mediasource/`) is the
  template, and the port's two classes are as thin: a second copy of MSE's
  rules would be a defect.
- **`MediaSourcePrivateKosmos`** makes source buffers and tells the player
  of a new duration; **`SourceBufferPrivateKosmos`** does the work, with a
  `MediaPlatformType::Kosmos` from patch 0028.
- **A demux thread a source buffer** reads what has been appended through
  `struct media_io` as one file arriving slowly, with a new door in the
  Media Kit: **`media_open_appended`**, which reads the container's header
  and nothing past it - `media_open` reads on until it knows every
  parameter of every stream, which is seconds of film a MediaSource has not
  been handed yet. **An append is finished when the demuxer asks for bytes
  and there are none**: what it made has gone to the page's thread first,
  in order. A WebM block seldom says how long it lasts, so a sample is held
  until the next of its track measures it.
- **A decode thread a source buffer** decodes what WebCore enqueues - the
  Media Kit's `media_decode` takes a packet it did not make, at WebCore's
  times, which the timestamp offset has moved - and hands each frame to the
  player's queue of M2, which shows it when its time comes. A sound
  SourceBuffer's samples are kept and counted until M4.
- **The player in MSE** takes its ready state, duration and buffered ranges
  from the MediaSource, seeks as WebCore's mock does (wait for the target
  to be buffered, enqueue again from the key picture before it), and its
  clock waits at the end of what is buffered.
- **What it says it plays** to `MediaSource.isTypeSupported`: WebM and MP4,
  video and audio, with the codecs of M2 - and not 10-bit VP9 or AV1, which
  are decoded and not yet shown, so YouTube chooses an 8-bit stream rather
  than a black window.
- **Test, in `x86-jetski`**: beside M2's film, a second whose picture (VP9)
  and sound (Opus) are separate WebM files made by FFmpeg at the start,
  each fetched and appended in pieces cut at arbitrary bytes, then
  `endOfStream()`; it must play, be its size, have its six seconds
  buffered, and show its own colour. Control: its picture served as a 404.
- **Not in M3**: `abort()` mid-segment drops the unread bytes but not what
  FFmpeg holds of a box half read (YouTube appends whole segments);
  `changeType()` to another codec; and a quality switch, which is a new
  initialization segment in the same buffer.

**M3 built, 14 November 2026** (0.11.420, `testing.md` 18.815): a film
appended to a MediaSource in pieces plays in JetSki, picture and sound in
SourceBuffers of their own, held by `x86-jetski`. `media_open_appended`
takes the container `addSourceBuffer` declared and holds the first bytes
to it, which the control found was needed: FFmpeg's matroska reader waits
on garbage rather than refusing it. Next, YouTube itself (M5's first look,
before M4's sound): what its player asks of the browser that is not here.

**YouTube plays, 14 November 2026** (0.11.421, `testing.md` 18.816):
"Me at the zoo" to its end in JetSki, AV1 and Opus through MSE, and the
next video's ad after it - picture only. Its trace found the clock
skipping film when a page's script held the thread past a stall; it now
waits where the picture ran out, held by `x86-jetski`. What YouTube still
lacks is M4's sound, and on real cores how fast AV1 decodes at 1080p.


## M4, as it is being built: the sound, and the clock

- **One Opus decoder.** The architecture said "Playback Kit + Opus", and
  the Playback Kit's core decodes WAV, MP3 and AAC itself. But the Media
  Kit already decodes Opus, Vorbis, AAC and FLAC - FFmpeg's - and a second
  Opus in the Playback Kit would be the second copy the premise forbids.
  So **the sound source buffer's decode thread decodes with the Media
  Kit**, as the picture's does, and hands the Playback Kit 16-bit PCM, the
  kind it calls WAV: its thread in the audio band converts to the device's
  rate and writes into a stream of the audio server's. The input ring is
  a megabyte, five seconds of 48 kHz stereo, so the decode thread may be
  away that long and nothing is heard missing. M6 finishes the thought:
  Video decodes with the Media Kit too, and the Playback Kit's own AAC
  goes with the vendored closure.
- **The Playback Kit has a door from C** (`playback.h`): its player - the
  thread, the input ring, the commands, what is heard - moved out of the
  Lua binding into `playback_player.c`, and `playback_kosmos.c` is that
  door with Lua's arguments around it.
- **A stream from C** (`user/kits/audio/audioclient.h`): what `audio.lua`
  does - the ring made here, its capability handed to the server with
  `open` - for code with no `lua_State`. `sys.ring_create` is the same
  function underneath, so a ring is made one way.
- **The web process is handed `/Devices/audio`**: the runner gives a C
  program its `/Devices/audio` with the Storage Kit's mounts
  (`sys.storage_start`, proto "audio"), and JetSki's launcher gives it to
  a web process - not to the network process, which plays nothing.
- **The clock is the sound's**: the film's time when the first sound
  since the last seek went in, plus the frames the device has played of
  that stream (`frames_played` in its ring). Each seek opens a new stream,
  which drops what the old one had queued. When the picture runs out - a
  stall - the sound is paused and the clock holds where the picture ended.
  Without sound it is the counter, as before.
- **Volume and mute** are the stream's fader at the audio server.
- **Test, in `x86-jetski`**: the suite's machine gets a sound card whose
  output QEMU writes to a WAV; the appended film's sound is a 440 Hz tone,
  and seconds of it at that pitch must come out. The control appends a
  silent track instead.
- **Owed**: a film with picture and sound in one source buffer is shown
  and not heard (one decoder a buffer); `<video src>` has no sound; a
  pause plays out what the stream's ring holds, about a fifth of a second;
  a sound whose rate or channels change part way stops at the change; the
  web process's sound thread is not granted the audio band yet
  (`kosmos: needs audio`), so it runs at NORMAL with the input ring as its
  margin.

**M4 built, 14 November 2026** (0.11.422, `testing.md` 18.817): the
appended film's tone heard from the sound card in `x86-jetski`, with a
silent track and a muted film as its controls. Next, by Diego's order of
the same day: M6, Video on the Media Kit, then M5.

## M6, written before it is built: Video on the Media Kit, in `media.elf`

**Why now.** `media.elf` is the image the media family shares (Cut's
decision 3, `videoedit.md`; Paint's, `photoedit.md`), and the new Music
cannot start without it (`player.md` P0). M6 is what makes it, with Video
as its first application.

**What it is today.** Video is an application in the system's image. Its
kit is `video.lua`, which reads MP4 with `mp4.lua` and decodes with the
vendored FFmpeg closure (`runtime/upstream/ffmpeg/`, `user/kits/ffmpeg/`,
`/Kosmos/Kits/h264`). Its sound goes through the Playback Kit, whose AAC is
that closure's too (`aac_core.h`). Three things call it: the Video app, the
`listen` program (which `x86-film` and `x86-film-load` play through), and
the old Music's `media.lua`, for an M4A's sound.

**What M6 does**, as recommended below:

1. **`media.elf`, a system image beside `webkit.elf`.** It lives in
   `/Home/System/Media`, is seen read only as `/Kosmos/Media`, is named by
   `-- kosmos: image media` (`IMAGES.SYSTEM` in `init.lua`), is a `system`
   package of its own, and is built by `make apps` (`tools/installed.py`).
   It carries the runtime, `gfx`, the UI kit, the Playback Kit, and **the
   Media Kit** - the whole FFmpeg with its assembly, dav1d, and the POSIX
   layer it stands on, which is why it is an image of its own and never the
   system's (`CLAUDE.md`, *Compatibility inside a process*).
2. **The Media Kit's Lua door**, `/Kosmos/Kits/media`, present only inside
   `media.elf`: a container opened from a file in the namespace, its tracks,
   packets in and frames out, a picture written into a surface (never a Lua
   string, `gfx.md` §19.1), and sound as PCM for the Playback Kit. It is
   C, as the decoders are.
3. **`video.lua` on that door.** It stays the kit applications call -
   `video.open(path)`, `film:draw`, `film:tick`, every call site the same -
   with the container and the decoders from the Media Kit. That means every
   format the kit reads (MP4, WebM, Matroska; H.264, VP9, AV1; AAC, Opus,
   Vorbis, FLAC) rather than MP4's H.264 and Motion JPEG. Asked from an
   image without the kit, `video.open` refuses, and says which image it
   needs. Its sound goes to the Playback Kit as PCM from the Media Kit (as
   M4's does for JetSki), so the Playback Kit's own AAC has no caller in
   `media.elf`.
4. **Video and `listen` move into it.** Video becomes a system application
   in `/Home/System/Apps/Video`, as JetSki is (`ns.SYSTEM_APPS`), and its
   header names `-- kosmos: image media`; `listen` names the image too.
   Tracker opens a film with Video, as it does now.
5. **Big Buck Bunny first** (`roadmap.md`, *A media machine*): a 1080p
   H.264 and AV1 copy, fetched as test data, played in the window.

6. **The closure and `mp4.lua` leave the tree** (decision 2, Diego's (b)):
   `runtime/upstream/ffmpeg/`, `user/kits/ffmpeg/`, `/Kosmos/Kits/h264`,
   `tools/ffmpeg_vendor.py`, `mp4.lua` and the Playback Kit's own AAC. Its
   other caller, the old Music's M4A sound, stops: `media.lua` refuses an
   M4A and says the new Music will play it, until the new Music's P5
   retires the old one. MP3, WAV and FLAC play in the old Music as before.
   One FFmpeg in the tree from then on.

**Tests**: `x86-film` and `x86-film-load` play through `listen` in
`media.elf`; Video's display check opens a WebM (VP9 and Opus) as well as
an MP4; each with the controls they have, and a new one - `video.open`
from the system's image must refuse, and say `media`.

### M6a built: `media.elf` and the Lua door

**0.12.94** (`testing.md` 18.1015). `media.elf` is linked by `make apps`
from `FULL=0`'s userland, the Media Kit, its door
(`user/kits/media/media_kosmos.c`), the POSIX layer and FFmpeg built
whole. It is 63 MB with its debugging sections, and the system's image is
checked to hold neither the door nor FFmpeg. `installed.py` puts it in
`/Home/System/Media` with a `package.txt`, a `system` package of its own
(`packages.media_facts`). init mounts that folder read only as
`/Kosmos/Media` and knows `-- kosmos: image media` (`IMAGES.SYSTEM`). The
door is `media.version()`, `media.open(path)`, and a source's `streams()`,
`read()`, `seek(us)` and `close()`. A packet is a userdata over the kit's
own, freed by `free` or the collector, and its bytes never become a Lua
string. The file comes through the Storage Kit, so a source reaches what
its program's `fs` does.

### M6b built: decoding from the door

**0.12.95** (`testing.md` 18.1016). `src:decoder(i [, threads])` gives a
decoder for one stream. It holds its source, so the source lives as long
as the decoder.

* `dec:send(packet)` takes the next packet, and `dec:send()` with none, at
  the end, makes the decoder give up the frames it still holds. It
  answers false when its frames must come out first; the caller takes
  them out and sends the same packet again.
* `dec:picture(surface)` draws the next picture straight onto a surface,
  from the decoder's planes, with `gfx_draw_i420`, and returns its time.
  10- and 12-bit pictures are narrowed to 8 bits in buffers the decoder
  keeps. 4:2:2, 4:4:4 and NV12 are refused with a sentence until
  something plays one. Pictures 720 lines or taller are taken as BT.709,
  smaller ones as BT.601, studio range.
* `dec:sound()` turns the next sound into 16-bit interleaved PCM, rounded
  and clipped, in a buffer of the decoder's own. It returns that buffer's
  address and length, the sound's time and its frames. That is the shape
  `player:frame(at, n)` takes, so a sound reaches the Playback Kit without
  becoming a Lua string.
* A time is `math.mininteger` where the film says none. Both `picture` and
  `sound` return false when the decoder wants another packet, nil when
  everything has come out, and nil with a reason on a fault.

Next: M6c, `video.lua` on the door, with Video and `listen` in
`media.elf`.

### Decided (M6)

**Diego, 23 November 2026**: (1) **(a)**, `media.elf` a system image
beside WebKit's, as recommended; (2) **(b)**, the closure and `mp4.lua` out
at M6 rather than kept until P5, the old Music without M4A in between.

### The decisions as they were asked (M6)

1. **Where `media.elf` lives.** (a) A system image in `/Home/System/Media`,
   seen as `/Kosmos/Media`, as WebKit is *(recommended)*: Video, Music,
   Cut and Paint are the system's applications, and one image serves them.
   (b) An installed application's image in `/Home/Apps`, as Doom's is.
2. **The old Music's M4A sound until the new Music's P5.** (a) Keep the
   closure and `mp4.lua` in the system's image until P5 retires the old
   Music *(recommended)*: no work on code that is going. (b) Take them out
   at M6, and the old Music stops playing M4A until P5. (c) Move the old
   Music into `media.elf` at M6 too.

