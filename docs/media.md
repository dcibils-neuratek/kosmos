# Video and sound in JetSki: YouTube's player on Kosmos

Written 14 November 2026, for Diego to decide, after: "YouTube is my most
used website so it must work perfect", "Watching videos in youtube as is a
top Priority", "So we need to have all decoders needed available"
(`roadmap.md`, *YouTube first, and every decoder*). Nothing here is built.

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
