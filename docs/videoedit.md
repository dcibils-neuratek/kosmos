<!-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. -->
# Kosmos Cut: a video editor

Diego, 22 November 2026: "i want to build a fully equiped video editing app
like adobe premiere or imovie. so research on what those are and their
funciontality and lets work on a spec and a mockup for it."

This document is the feature set, what other editors do, the architecture,
the busiest paths, the steps and the decisions. `videoedit.html` is the
window and `videoedit-architecture.png` is what it stands on, drawn from
`videoedit-architecture.html`. It was written on 22 November 2026, before
any of it is built, and is **for Diego to agree or change**. *Kosmos Cut*
is a working name (decision 1).

---

## In short

- **One application, two depths.** *Simple* is iMovie's ground: one story
  line, music under it, titles, transitions, a look, and a film out. *Full*
  is Premiere's: as many tracks as you want, the trimming tools, keyframes,
  colour with scopes, a mixer, multicam and proxies. Both views show the
  same project, so a film started simple can be finished in full.
- **The application is thin.** Reading and decoding films is the **Media
  Kit**, which exists. The sound plays on the **Playback Kit**, which
  exists. Pixels are the **Graphics Kit**'s, which grows colour, LUT and
  keying primitives that Photo wants too. Three things are new and each is
  a kit with one door, because another application wants it:
  - **the Timeline library** (`timeline.lua`): tracks, clips, the edits and
    undo;
  - **the Edit Kit** (`/Kosmos/Kits/edit`, C): the frame and the sound at a
    moment of a timeline, the same code for the viewer and for export;
  - **encoding in the Media Kit**: x264, FFmpeg's AAC and FFmpeg's muxers,
    behind the same `media.h`.
- **No pixel and no sample passes through Lua.** Lua says what the timeline
  is. C renders it on threads into a ring of frames in a region. The window
  shows the newest frame that is due, and the clock is the sound's.
- **Scrubbing is the hard part, not playing.** A long-GOP H.264 film can
  need a hundred frames decoded to show one. So there is a frame cache,
  decode-ahead, and proxies where every frame is a key frame. The viewer
  renders at its own size, not the film's.
- **CPU first, held on the Mac; then Vulkan compute** for the effects, on
  lavapipe in QEMU and `anv` on the M700, held to the CPU path. Hardware
  decode and encode on the M700 come later, as `m700-gpu.md` describes.

---

## 1. What other editors do

Researched on 22 November 2026, from each product's own documentation (see
*Sources* at the end). This is a summary in my own words.

### The six, in a line each

- **Apple iMovie.** A *project* is one movie with a library of events. There
  is a single main line of clips. Over it go cutaways, picture in picture,
  green screen and split screen, and under it go music and voice. There is a
  small set of transitions and title styles. *Themes* dress a whole film and
  *trailers* fill a template's shot list. Each clip can be cropped, given a
  Ken Burns move, stabilised, colour-balanced, colour-matched, sped up or
  slowed. A *precision editor* handles a cut's two sides. Export goes to a
  file at a few sizes.
- **Adobe Premiere Pro.** Bins and a source monitor feed sequences on
  unlimited video and audio tracks. Its tools panel has Selection, Track
  Select, Ripple, Rolling, Rate Stretch, Razor, Slip, Slide, Pen and Hand.
  Other features:
  - three-point editing with insert, overwrite, lift and extract;
  - linked audio and video, nested sequences, markers;
  - an effect stack with keyframes on any parameter;
  - Lumetri colour (basic correction, curves, wheels, HSL secondaries,
    LUTs) and the Lumetri scopes: waveform, parade, vectorscope, histogram;
  - an audio track mixer with keyframed volume;
  - Essential Graphics for titles;
  - Warp Stabilizer, time remapping, multi-camera sequences;
  - proxies made at ingest and switched with one button;
  - export through Media Encoder's queue.
- **Apple Final Cut Pro.** The *magnetic timeline* has no tracks. Clips on
  the *primary storyline* close up when one is removed. Other clips
  *connect* to a primary clip and move with it, and connected clips group
  into secondary storylines. *Compound clips* nest, *roles* label what a
  clip is (dialogue, music, effects) for mixing and export, and *gap clips*
  hold time. It also has multicam, proxies and optimised media.
- **Blackmagic DaVinci Resolve.** Seven pages, one for each stage: Media,
  Cut (a fast editor), Edit (a full one), Fusion (node compositing), Color
  (the node-based grader the product grew from), Fairlight (a whole audio
  workstation) and Deliver (render queue).
- **Kdenlive.** A KDE front end over the MLT framework, with FFmpeg,
  frei0r and LADSPA underneath. It has multitrack editing, proxy clips,
  keyframeable effects with linear or smooth curves, guides and markers,
  and a render dialog of profiles.
- **Shotcut.** Also on MLT. It edits files natively with no import step and
  mixes resolutions and frame rates in one project. Its features include:
  - ripple and three-point editing, and keyframes with easing;
  - speed ramps, reverse and freeze frame;
  - video scopes (histogram, parade, waveform, vectorscope) and audio
    scopes (loudness, peak, spectrum);
  - colour wheels and LUTs, automatic ducking, proxies;
  - export to many codecs, as EDL, or as chapters;
  - a project file in MLT's XML, with autosave.

### What every one of them has

| Area | What it means |
|---|---|
| **Library** | Media imported (or only referenced), thumbnails, a filmstrip to skim, favourite or reject a range, search, bins or keywords, and the clip's facts. |
| **Viewer** | Play, pause, frame step, J K L shuttle (back, stop, forward, faster each press), in and out marks, a timecode, and fit or 100%. Full editors add a second *source* viewer. |
| **Timeline** | Tracks (or a storyline), clips with thumbnails and waveforms, a playhead, zoom, snapping to edits, markers and the playhead, linked audio and video, track lock, mute and solo. |
| **Edits** | Append, insert, overwrite, replace, lift (leave a gap) and extract (ripple delete), split at the playhead (the razor). |
| **Trims** | *Ripple* moves an edge and shifts everything after it. *Roll* moves a cut between two clips, so one grows as the other shrinks. *Slip* changes which part of the source a clip shows, keeping its place. *Slide* moves a clip between its neighbours, which trim to make room. *Rate stretch* changes a clip's length by its speed. |
| **Structure** | Nesting or compound clips, multicam, adjustment layers. |
| **Transitions and titles** | Dissolves, dips, wipes and pushes with a length. Titles from styles, as a lower third, centred, or rolling credits. |
| **Effects and keyframes** | A per-clip stack: transform, crop, opacity and blend mode, blur, sharpen, chroma key, masks. Any parameter can be keyframed, with easing, on a curve editor. |
| **Colour** | Exposure, contrast, temperature, saturation. Lift, gamma and gain wheels, curves, HSL secondaries, LUTs, shot matching. Scopes: waveform, RGB parade, vectorscope, histogram. |
| **Audio** | Waveforms, clip gain, fades, keyframed volume (the "rubber band"), a mixer with faders, pan and meters, EQ, compressor and noise reduction, loudness normalisation, ducking music under voice. |
| **Time** | Constant speed, reverse, freeze frame, speed ramps (time remapping), optical-flow or frame-blended slow motion. |
| **Repair** | Stabilisation, rolling-shutter repair, deinterlace. |
| **Performance** | Proxies, a render cache of heavy sections, preview at reduced resolution, hardware decode. |
| **Output** | Presets (H.264 or HEVC in MP4 at 1080p or 4K, ProRes, AV1), a queue, a frame as a still, chapters from markers, an EDL or XML for another editor. |
| **Safety** | Unlimited undo, a history list, autosave, relink of moved media, a project file. |

### What makes iMovie simple

- **One line, not tracks.** A movie is a sequence of clips, and anything
  over or under it is attached to one of them. Removing a clip closes the
  gap. There is nothing to target and nothing to lock.
- **Choices are styles, not parameters.** A title is a style and a text. A
  transition is a kind and a length. A look is a filter with one slider.
- **The defaults are the film.** Automatic transitions, a theme, the audio
  levelled, and stabilisation is a checkbox.
- **Export is one sheet**: a size, a quality, a file.

### What makes Premiere "fully equipped"

- **Tracks you target**, and edits that respect that: three-point editing
  from a source viewer, insert and overwrite, sync locks.
- **The trimming tools** - ripple, roll, slip, slide, rate stretch - each a
  precise operation on a cut, not a drag that guesses.
- **Every parameter keyframed** with curves.
- **Colour as a discipline**: wheels, curves, qualifiers, LUTs, measured on
  scopes rather than by eye.
- **Audio as a mix**: tracks, buses, meters, loudness to a target.
- **Scale**: nesting, multicam, proxies, a render queue, long projects
  without slowing.

So the tiers below put iMovie's ground first, as a film a person can
finish, and Premiere's on top, in steps.

---

## 2. What a person can do (the feature set)

### Tier 1: a first editor worth using (Simple)

- **Start a movie** from the Welcome sheet: name it, choose its size and
  rate (1080p at 30 by default, or the first clip's), and it is a folder in
  `/Home/Movies`.
- **Bring in clips** by dragging them from Tracker, or through Import... (the
  Open window, `panel.lua`): MP4, MOV, WebM and MKV films, MP3, WAV and FLAC
  sound, PNG and JPEG photographs. They are referenced where they are, not
  copied (decision 4). Camera recordings in `/Home/Captures` appear as their
  own place.
- **The library**: each clip as a thumbnail with its length. Hover to skim
  it in the viewer. Press I and O, or drag across the filmstrip, to choose a
  range. Favourite (F) or hide (Delete) a range. Search by name.
- **Build the story**: E appends the chosen range to the end of the story
  line, W inserts at the playhead, Q connects it above (a cutaway or picture
  in picture), and dragging does any of these. The story line is magnetic:
  removing a clip closes the gap.
- **Music and voice**: a sound dropped under the story line plays under it.
  Record voice from the machine's input (`usbaudio.md` U9).
- **Trim**: drag a clip's edge (a ripple trim), or press S to split at the
  playhead. Snapping to edits and the playhead is on and N turns it off.
- **Transitions**: cross dissolve, dip to black, dip to white, wipe, push
  and slide, from a short list in the inspector. Each has a length and goes
  on a cut by double-click or Command T.
- **Titles**: six styles - Lower third, Centred, Headline, Caption, Credits
  (rolling) and Chapter - each a text, a face (the four IBM Plex faces and
  whatever is on the disk), a colour and a position. Titles are drawn by the
  Graphics Kit's text.
- **Per clip**:
  - volume and fade in or out;
  - fit, crop, or Ken Burns (a start and an end frame) for a photograph;
  - rotate and flip;
  - speed (¼, ½, 1, 2, 4×), reverse, freeze frame;
  - a look: one of eight colour presets with a strength slider, plus
    exposure, contrast, saturation and warmth.
- **See and hear it**: Space plays and pauses, J K L shuttles, the arrows
  step a frame, and the waveform shows under every clip with sound. A meter
  by the viewer shows left and right.
- **Undo everything**: Command Z and Shift Command Z, without limit while
  the project is open. Every change saves itself.
- **Export**: one sheet - a name and a place, 1080p, 720p or the original
  size, quality Good, Better or Best - H.264 and AAC in MP4, with its
  progress and an estimate of the time left. The machine stays usable while
  it runs.

### Tier 2: the fuller editor (Full)

- **Tracks**: any number of video and audio tracks, each with its own
  height, name, lock, mute, solo and *target* switch. The story line of
  Simple is track V1. Clips above layer in order.
- **The tools**, on the toolbar and as keys:
  - Select (A) and Track select forward (Shift A);
  - Ripple (B) and Roll (R);
  - Slip (Y) and Slide (U);
  - Rate stretch (X) and Razor (C);
  - Hand (H) and Zoom (Z).

  Each acts on the edit nearest the pointer and shows the two frames either
  side of the cut in the viewer while dragging, as the Trim view.
- **Three-point editing**: a source viewer beside the program viewer, in and
  out on either, and insert (comma) or overwrite (full stop) onto the
  targeted tracks. There is also lift and extract of a timeline range.
- **Linked clips and split edits**: a film's picture and sound move
  together until unlinked (Command L). Their edges can be trimmed apart,
  which is a J cut (the sound leads) or an L cut (the sound lags).
- **Markers**: on the timeline or a clip, named and coloured, which become
  chapters on export.
- **Nested sequences** (compound clips): select clips, Command G, and they
  become one clip that opens to its own timeline.
- **Keyframes on every parameter**: a diamond beside each slider in the
  inspector, a lane under the clip, and a curve editor with linear, ease
  in, ease out and Bézier handles.
- **The effect stack**, in order, per clip or on an adjustment clip over
  several:
  - transform (position, scale, rotation, anchor) and crop;
  - opacity and blend mode (normal, add, multiply, screen, overlay);
  - Gaussian blur, sharpen;
  - chroma key (green or blue, with spill suppression);
  - rectangle and ellipse masks with feather;
  - drop shadow, vignette;
  - stabilise (later, step V14).
- **Colour**:
  - exposure, contrast, highlights, shadows, temperature and tint,
    saturation and vibrance;
  - lift, gamma and gain wheels with offset;
  - RGB and luma curves, and HSL secondaries (choose a hue and correct only
    it);
  - a LUT from a `.cube` file;
  - a reference frame to match to;
  - the scopes: waveform, RGB parade, vectorscope and histogram, live under
    the viewer.
- **Audio**:
  - a mixer with a strip a track - fader, pan, mute, solo, meter - and a
    master with a loudness reading;
  - clip gain, and volume keyframes drawn on the clip (the rubber band);
  - per-track EQ (four bands), compressor and noise gate;
  - normalise to a loudness target (-14 LUFS for the web, -23 for
    broadcast);
  - ducking: music lowered under a chosen voice track.
- **Time**: speed ramps - speed keyframed over a clip, with the clip's
  length following - and frame blending for slow motion.
- **Multicam**: choose clips of one event, and they are lined up by their
  sound (cross-correlation of their waveforms). Play the angles side by
  side, and press 1 to 9 to cut between them live. Each cut is a clip
  you can roll after.
- **Proxies**: made in the background at import, at half or quarter size,
  with every frame a key frame. One switch in the viewer picks proxies or
  originals, and export always uses the originals.
- **Render**: a heavy section - many layers, a key, a LUT on 4K - can be
  rendered ahead into the project's cache. A red bar over the ruler marks
  sections that will not play in real time, and green marks rendered ones.
- **Export, fully**:
  - H.264 or AV1 in MP4, MKV or WebM, at any size and rate, by a quality
    or a bit rate, with sound in AAC, Opus or FLAC;
  - a still frame as PNG, or the sound alone;
  - a queue of exports, which goes on while you edit;
  - markers as chapters, and subtitles from a subtitle track as SRT;
  - two-pass export (decision 6).
- **History**: the list of every change, any of which can be returned to,
  and the project's autosaves by time.
- **Relink**: a clip whose file moved is shown hatched and found again by
  its name and size.

### Tier 3: later, written down so they are not forgotten

Motion tracking, optical flow, HDR and 10-bit end to end, node compositing
(Resolve's Fusion), transcription into subtitles (Xavier), a template for a
trailer, a theme for a whole movie, collaboration, and an EDL or FCPXML
exchange.

### Left out, on purpose

- **A second application for Simple.** One application with two views, so
  a project never has to be moved from one to the other (decision 2).
- **Plug-ins** (OpenFX, VST). The effects are the Edit Kit's, in C. A
  plug-in system is a later question.
- **Hardware codecs in the first steps.** The M700's engine decodes H.264
  and encodes it (`m700-gpu.md`), but software comes first and is measured.

---

## 3. What it is made of

**The application is the thinnest layer** (`CLAUDE.md`, *Kits, servers and
drivers supply; applications orchestrate*). Each piece below was asked the
two questions: does another application want this, and does something
already supply it?

| Piece | Supplied by | New, grows, or as it is |
|---|---|---|
| The window: header, sidebar, lists, cards, tabs, sliders, dropdowns, menus, sheets | the UI kit, `ui.lua` | as it is |
| The viewer, thumbnails and waveforms shown in a kit window | `ui.lend_picture` and `gc:lent` - a surface this process lent the painter, drawn with no copy (as GPU Fractal's) | as it is |
| Choosing files | the Open window, `panel.lua`; dropping from Tracker, `ui.drag` and `ui.dropped` | as it is |
| Settings and the recent projects | `prefs.lua`, `recentdocs.lua` | as it is |
| "Export finished" | `notify.lua` | as it is |
| A project's file type and icon | `filetypes.lua` | grows: `.cut` |
| Reading films: containers and decoders | **the Media Kit**, `user/kits/media/media.h` - WebM, MP4, Ogg, MP3, FLAC, ADTS and WAV, decoded by FFmpeg 9.0.2 (built whole with its assembly) and dav1d | as it is |
| **A frame at an exact time**, a thumbnail, a waveform's peaks | **the Media Kit**, three new calls (below) | **grows** - Video's seek, Tracker's preview (`preview.lua`), Music and the Open window want them too |
| **Writing films**: encoders and muxers | **the Media Kit**, an encode door: x264 for H.264 (decision 5), FFmpeg's `aac` and `flac` encoders, libopus or FFmpeg's `opus`, FFmpeg's `mov`, `matroska` and `webm` muxers | **grows** - the Record Kit moves onto it (one H.264 encoder), and Camera, a screen recorder and Sandboxes' capture want it |
| PNG of a still | the Graphics Kit, `gfx.encode_png` | as it is |
| YUV to the screen's pixels | the Graphics Kit, `gfx_i420` (BT.601 and BT.709, SIMD) | as it is |
| **The screen's pixels to YUV**, for the encoder | **the Graphics Kit**, `gfx_rgb_i420`, SIMD, beside `gfx_yuy2_i420` | **new**, small - Camera's effects and a screen recorder want it |
| Scaling, blending, blur, tint, text, shadows | the Graphics Kit's surfaces: `stretch`, `blend`, `blur`, `tint`, `text`, `shadow` | as it is |
| **Colour and keying primitives**: a grade (lift, gamma, gain, saturation), curves as a 1D table, a 3D LUT, an HSL qualifier, a chroma key, a mask with feather | **the Graphics Kit**, one file, `grade.c`, held to a scalar reference on the Mac as `yuv.c` is | **new** - Photo wants every one of them for stills, and Camera a look |
| **The timeline**: tracks, clips, ranges, links, markers, keyframes; the edits (append, insert, overwrite, ripple, roll, slip, slide, stretch, razor, lift, extract, nest); undo and history; the project file | **the Timeline library**, `user/lib/timeline.lua` | **new** - a podcast or sound editor, Kosmos Present's video on a slide, and Automations want the same model |
| **The frame and the sound of a timeline at a moment**: which clips are live, the source time of each (speed, ramps, reverse), decoding ahead on threads, a cache of decoded frames, every layer's effects, transitions and titles, and the mix of every audio track with its gain, pan, fades, EQ and compressor | **the Edit Kit**, `/Kosmos/Kits/edit`, `user/kits/edit/`, C | **new** - Present's transitions, Camera's recording with a title, and any program that renders a film from a description |
| Sound played in the audio band, the clock | **the Playback Kit**'s C door, `playback.h`, handed the mix as 16-bit PCM (its WAV kind), as JetSki's web process does | as it is |
| Converting the mix to the device | `pcmconvert.h`, inside the Playback Kit | as it is |
| A stream to the audio server from C | `user/kits/audio/audioclient.h` | as it is |
| EQ, compressor, gate in the mix | **the Edit Kit**'s audio half; the Synth Kit's state-variable filter and reverb (`synth_dsp.h`) are the model, and a biquad is shared with Music's equaliser when that is built (`music.md` M4) - one biquad, in a header both include | **new**, one copy |
| Loudness (EBU R128) | **the Edit Kit**, a small C meter. FFmpeg's `ebur128` is a filter, and filters are not built (`--disable-avfilter`) | **new** - Music's normalisation and the Mixer's meter want it |
| Effects on the GPU | **the Vulkan Kit**, `kvk.c`'s compute pipelines (Xavier's, `kvk_compute_make`), on lavapipe in QEMU and `anv` on the M700 | as it is; the Edit Kit's shaders are new |
| Hardware decode and encode | the M700's video engine (`intelgpu`, on execlists since G6c); the codec is `m700-gpu.md` steps 5 to 8, not built | later, behind `media.h` |
| Files read and written | the Storage Kit and the POSIX layer inside the image (`w3.md`): `read` into the kit's own memory, and export written at an offset (`DISK_OP_WRITE_AT`, which the Storage Kit already sends; `diskfs.md` *Writing at an offset*) | as it is |

**Does something already supply it? Three near misses, said so they are
not copied:**

- **`video.lua` and `mp4.lua`** play a film for the Video app on the
  vendored closure (`user/kits/ffmpeg/`, H.264 and AAC only). They are not
  the editor's reader: the Media Kit is, and `media.md` M6 moves Video onto
  it and removes `mp4.lua`. The editor comes after M6 (or with it), so there
  is one reader.
- **The Record Kit** (`user/kits/record/`) encodes H.264 with lieff's
  `minih264e` (Baseline, one thread) into an MP4 held whole in memory by
  `minimp4`. That is right for a camera at 640 by 480, and not for a 1080p
  film of ten minutes. The editor's encoder goes into the Media Kit, and the
  Record Kit moves onto that door when it is there. If decision 5 chooses
  `minih264e` instead, it moves into the Media Kit from the Record Kit, and
  still there is one copy.
- **`media.lua`** is Music's engine and has nothing to do with this. It is
  renamed with M6, as `media.md` notes.

### The doors

```lua
-- The Timeline library: data and edits, no pixels.
local timeline = use("/Kosmos/Libraries/timeline.lua")
local tl = timeline.new{ width = 1920, height = 1080, rate = { 30, 1 } }
local id = tl:add_media("/Home/Films/harbour.mp4", facts)  -- facts from the Media Kit
tl:append(id, in_us, out_us)          -- and insert, overwrite, connect
tl:ripple(edge, delta_us)  tl:roll(cut, delta_us)
tl:slip(clip, delta_us)    tl:slide(clip, delta_us)
tl:stretch(edge, delta_us) tl:razor(track, at_us)
tl:undo()  tl:redo()  tl:history()
tl:save(path)  timeline.open(path)
tl:describe()   -- the render description, flat, for the Edit Kit

-- The Edit Kit: a timeline at a moment, from C.
local edit = use("media.elf").edit     -- the image's door (decision 3)
local e = edit.open{ out = region, frames = 3, width = 960, height = 540 }
e:set(tl:describe())    -- after every edit: the layers, effects and mixes
e:show(t_us)            -- a scrub: newest wins; a frame appears in the ring
e:play(t_us)  e:pause() -- frames rendered ahead; sound through the Playback Kit
e:frame()               -- the newest due frame's slot, or nil: lend it to the viewer
e:position()            -- the sound's time
e:export{ path = ..., preset = ... }   -- on threads of its own; e:progress()
```

`media.h` grows in the same shape as now (C, microseconds, nothing of
FFmpeg showing):

- `media_frame_at(src, dec, us, out)`: the frame shown at `us`. It seeks to
  the key frame before and decodes forward, and the caller's cache decides
  whether it needed to.
- `media_thumbnail(src, us, w, h, out)`: a small picture for a strip.
- `media_peaks(src, stream, per_second, out, n)`: a waveform's minimum and
  maximum per bucket, made once and kept in the project's cache.
- `media_writer_open(io, container, ...)`, `media_encoder_open(kind, codec,
  quality, ...)`, `media_encode(enc, frame)`, `media_write(writer,
  packet)`: the encode door, the reverse of what exists.

### Where the code lives, and which process

- **The Media Kit stands on the POSIX layer**, since FFmpeg is built with
  threads (`roadmap.md`, M1's note). The POSIX layer goes into an image of
  its own and never into the system's (`CLAUDE.md`, *Compatibility inside a
  process*). So Cut is an **installed application** in Flight's shape:
  `user/installed/Cut/` holds its Lua, and its C is an image it names with
  `-- kosmos: image media.elf`.
- **One image for every application that edits or plays a film**
  (decision 3): `media.elf` carries the Media Kit, the Edit Kit, the
  Playback Kit and the POSIX layer. Video names the same image when M6 moves
  it to the Media Kit. Two images with the same kits in them would be two
  copies to build and keep in step.
- **One process.** The window is Lua on the main thread. The Edit Kit runs
  a render thread, the Media Kit's decoders run their own threads, and the
  Playback Kit runs its thread in the audio band (`kosmos: needs audio`).
  Export runs on threads of its own at LOW, so the desktop and the editor
  stay ahead of it.

### C or Lua

- **Lua**: the window, the timeline's model and its edits, undo, the
  project file, which clip is under the pointer, and snapping. These are
  structure, tables and rules. A bug there is a stack trace, and C would
  buy about 2% (`CLAUDE.md`, *Language split*). `timeline.lua` is tested on
  the Mac's Lua as `model.lua` is (`tools/test_flightmodel.lua`).
- **C**: everything per pixel and per sample. That covers decoding,
  conversion, scaling, compositing, colour, keying, transitions, titles
  rasterised, the mix, the meters, the scopes, thumbnails and waveform
  strips drawn into surfaces, and encoding. Loops over pixels use GCC's
  vector types, as `gfx.c`'s blend and `pack.c` are written, with the scalar
  loop kept as the reference a host test holds the vector one to.
- **GPU**: the per-pixel effects again, as Vulkan compute shaders through
  `kvk.c`, held to the C path's output on lavapipe and then on `anv`.
  Decision 7 sets when.

### What travels in a region and what in a message

(`CLAUDE.md`, *Control by message, data by shared memory*.) Most of this is
inside one process, so "a message" is mostly a call into a kit. The rule
still decides the shape.

- **Decoded frames** live in the Edit Kit's cache, in memory the kit maps,
  never in Lua. Frames are about 3 MB each at 1080p 4:2:0, and the cache is
  sized from free memory, 64 frames by default.
- **Rendered frames** go into **a ring of three surfaces in a region**. It
  is single-producer, single-consumer: the render thread writes, and the
  window reads the newest frame that is due and lends it to the viewer. That
  is the same shape as JetSki's ring of three in `media.md` M2, and a region
  so that the window could later be another process.
- **Thumbnails and waveform strips** are surfaces in the same kind of
  region, an atlas for each track, redrawn by C when the zoom changes. Lua
  only says where they go.
- **Sound**: the mix is written as PCM into the Playback Kit's input ring
  (one writer, one reader), and from there into the audio server's stream
  ring. It is never a message.
- **The timeline itself** crosses from Lua into the Edit Kit as **a
  declared shape**: `edit.h`'s fixed structs - a layer, a clip's source
  range and speed, an effect's kind and up to sixteen numbers, a keyframe -
  built by `tl:describe()` into a buffer and copied whole by `e:set`. It is
  inside one process, so it is not a server boundary, but it keeps the C
  side from walking Lua tables on its thread, which it may not.
- **The clock**: `e:position()` is the Playback Kit's frames heard, plus
  where the last seek landed (`playback.md`). It is read in microseconds and
  never in ticks (`CLAUDE.md`, *Two clocks*).
- **Export progress** is a counter the export thread writes and the window
  reads twice a second. It is not a message a frame.
- **Writing the file**: the muxer writes through a `struct media_io` that
  appends into a ring of 4 MB pieces. A writer thread sends each piece to
  `/Home` with `DISK_OP_WRITE_AT`, so the export never holds the whole film in
  memory, which the Record Kit does today.

### The project file

`/Home/Movies/<Name>.cut/` is a folder:

- `project.json`: the timeline, written by `json.lua` (decision 8) - tracks,
  clips by media id and range, effects and keyframes, markers, the export
  settings;
- `Media.json`: each file's path, size and time when it was changed, and
  its facts, so a moved file can be found again;
- `Proxies/`: the proxy films;
- `Cache/`: thumbnails, waveform peaks and rendered sections;
- `Autosave/`: the last ten `project.json`, one a minute while changes come.

It is data, read by a parser and checked field by field, never run (`CLAUDE.md`,
*No precompiled Lua bytecode*, and a project from another machine is
untrusted).

---

## 4. The busiest paths

**Numbers below that are not measured are labelled as estimates.** QEMU's
numbers are not speeds (`CLAUDE.md`), so the M700 measures every one in
the steps.

### Playing the timeline, with effects

The case: 1080p at 30 frames a second. Two video layers (a film, and a
picture in picture over it), a cross dissolve under way, a title, a colour
grade with a LUT, and three sound tracks. The viewer is 960 by 540. Each
frame has 33 ms.

1. **The render thread wakes** for the next frame time. It reads the
   description (no Lua) and finds, for each live clip, its source time
   (speed, reverse, ramps).
2. **The frames** come from the cache. While playing they were decoded
   ahead by the Media Kit's threads: H.264 at 1080p on FFmpeg with its
   assembly is *estimated* at 4-8 ms a frame on one Skylake core. Three
   sources are live during the dissolve, so that is spread over decode
   threads. M1's owed measurement settles it.
3. **To the viewer's size first.** Each layer is converted from 4:2:0 and
   scaled down to 960 by 540 in one pass (`gfx_i420` into a scaled target,
   SIMD). That is a quarter of the pixels for everything after, which is the
   one decision that matters most on this path. *Estimated* 1-2 ms a layer.
4. **Effects at viewer size**: the grade and the 3D LUT (trilinear, about
   20 operations a pixel), *estimated* 3-6 ms on one core with vectors, and
   split into four bands on four threads. Transform, opacity and the
   dissolve are blends the Graphics Kit already does at about a nanosecond a
   pixel. The title is a surface rasterised once and blended.
5. **Into the ring**, and a pulse to the window. The window lends the slot,
   the kit's painter draws it into the viewer view, and the window manager
   composes. The timeline view repaints only the playhead's columns and the
   timecode, not the tracks.
6. **The sound**, on its own clock: the Edit Kit's mix thread sums three
   tracks with gain, pan, fades and EQ into 16-bit stereo blocks of a
   twentieth of a second, ahead, into the Playback Kit's ring. Its thread
   in the audio band converts and writes periods, as for a film in JetSki.
   A period of three tracks is microseconds.
7. **Where the time goes**: decoding first, then the LUT, then conversion.
   A frame that would be late is dropped rather than shown late, as
   JetSki's player does, and the dropped count is shown in the viewer's
   corner while it is not zero. The red bar over the ruler marks sections
   predicted not to play, from the measured cost of each effect.

### Scrubbing

Dragging the playhead, or skimming a library clip, asks for frames out of
order. That is where every editor struggles.

- **Newest wins.** `e:show(t)` replaces any request not yet started. The
  render thread always works on the latest position the pointer gave, and
  never on a queue of old ones.
- **The cost is the GOP.** A phone's H.264 has a key frame every one or two
  seconds, so a frame can need 30 to 60 frames decoded first, and a camera
  with long GOPs needs more. *Estimated* 120-480 ms at 1080p, which is not
  scrubbing.
- **The answers, in order**:
  1. **the cache** - a frame decoded once is kept, and a scrub back over it
     is free;
  2. **key frames while moving fast** - while the pointer moves faster than
     the decoder, the nearest key frame is shown, and then the exact frame
     once it settles;
  3. **proxies with every frame a key frame** - x264 `keyint=1` at quarter
     size: any frame is one decode, *estimated* under 2 ms. This is the
     real fix, and why proxies are made at import in Full.
- **A thumbnail strip** for skimming a library clip is the kit's
  `media_thumbnail`, made once at import into `Cache/`. Skimming shows the
  strip's picture at once and the real frame when it arrives.

### Export

1. The same render function at the project's full size, on threads, from
   the originals (never proxies), with no frame dropped. The film can be
   slower than real time, and export waits for each frame.
2. **To YUV**: `gfx_rgb_i420`, SIMD, *estimated* 2 ms at 1080p.
3. **Encode**: x264 with its assembly, preset *medium*, constant quality
   (CRF 20 for Better), on four threads. *Estimated* 30-60 frames a second
   at 1080p on the M700's four cores, so about real time or better. This is
   the slowest stage, and why Good, Better and Best are x264 presets.
4. **Sound**: the mix again, in blocks, into FFmpeg's AAC encoder at 192
   kb/s. That is negligible.
5. **Mux and write**: FFmpeg's `mov` muxer with `+faststart`, through
   `struct media_io`. Faststart moves the index to the front, which is a
   second pass over the file (decision 6 covers two-pass video, not this).
   Then the writer ring and `DISK_OP_WRITE_AT`.
6. **The window** shows the frame being encoded, a bar, frames per second
   and the time left. Cancel stops at the next frame and removes the
   partial file.

---

## 5. Steps

Each step ends in something checked. The gate runs on x86-64 QEMU, and the
Mac runs host tests of pure C (and of Lua, on the host's interpreter). Films
for tests are made at the test's start by the Mac's FFmpeg from its own test
patterns and tones (`testsrc2`, `sine`). No site's bytes and none of
Diego's files go into the repository. Every check has a control that must
fail.

- **V0 - before any code**: Video on the Media Kit (`media.md` M6), so
  there is one reader, and the Media Kit's decode speed measured on the
  M700 (M1's owed number). Both are on the roadmap already.
- **V1 - a frame at a time.** `media_frame_at`, `media_thumbnail` and
  `media_peaks` in the Media Kit.
  - Host test: frames at 200 random times of a long-GOP H.264 and a VP9
    film, each held to `ffmpeg -ss T -frames:v 1 -f framemd5` on the Mac,
    picture for picture. The peaks are held to a scalar sum of the decoded
    samples.
  - Control: seeking without decoding forward from the key frame must fail.
- **V2 - the Timeline library.** `timeline.lua`: tracks, clips, every edit
  of tier 1 and tier 2 (ripple, roll, slip, slide, stretch, razor, lift,
  extract, insert, overwrite, connect, nest), undo and redo, and
  `project.json`.
  - Host test, `tools/test_timeline.lua`: every edit followed by its undo
    returns the identical project. Each edit is checked against a table of
    expected clip positions, the project round-trips through JSON, and a
    corrupt or hostile file is refused.
  - Control: an edit whose undo is deliberately wrong must fail.
- **V3 - the Edit Kit's render, on the Mac.** `user/kits/edit/edit_core.c`:
  layers, the source time, conversion and scaling, opacity, transform, the
  cross dissolve, a title. Also `gfx_rgb_i420` with its vector paths.
  - Host test, `tools/test_edit.c`, natively and through Rosetta: frames of
    a two-layer timeline held to a scalar reference. Every vector loop is
    held to its scalar loop, as `test_yuv.c` does.
  - Control: an off-by-one in the source time must fail.
- **V4 - the window, playing.** The image `media.elf` with the Edit Kit and
  the Playback Kit, and `user/installed/Cut/` with the library, viewer and
  timeline, playing a timeline of two films with sound.
  - Suite `x86-cut`: the test pattern's colours are found in the viewer on
    the screen, the position moves, and a 440 Hz tone is heard in the WAV
    QEMU's sound card writes (as `x86-jetski`'s M4).
  - Control: the film served as nothing.
- **V5 - editing tier 1 in the window**: import (Open window and drop),
  skim, I and O, append, insert, connect, trim, split, ripple delete,
  snapping, undo, autosave.
  - Suite `x86-cut-edit`: pointer and keys build a timeline that is then
    held to the expected `project.json`.
- **V6 - export.** The Media Kit's encode door: x264 and FFmpeg's AAC,
  `mov` and `matroska` muxers in `tools/w5_build.py`'s FFmpeg, the writer
  ring and `DISK_OP_WRITE_AT`, and the Export sheet.
  - Suite: a timeline exported and read back by the Media Kit inside QEMU,
    checking its length, size and the pattern's colours.
  - Host test: the same export decoded by the Mac's FFmpeg with PSNR above
    40 dB against the render's own frames.
  - Control: a frame skipped must fail.
- **V7 - Simple complete**: transitions, the six titles, speed, reverse,
  freeze, photographs and Ken Burns, crop and rotate, the looks and the four
  colour sliders (`grade.c` in the Graphics Kit, held on the Mac), fades and
  clip volume, the meter.
  - Pixel checks against the host reference; the fades are checked by their
    level in the recorded WAV.
- **V8 - scrubbing.** The frame cache, decode-ahead, newest-wins, key frames
  while moving fast, proxies made at import, and the proxy switch.
  - Measured under QEMU as a regression with `-icount`, and **on the M700
    as speed**: time from a pointer move to the frame on the screen, at
    1080p, originals and proxies.
- **V9 - Full: tracks and tools.** Tracks with target, lock, mute and solo;
  the tools (ripple, roll, slip, slide, rate stretch, razor, track select)
  in the window with the trim view; linking and J/L cuts; markers;
  three-point editing with the source viewer.
  - The tools' rules are already held by V2; here the suite drives each one
    by the pointer.
- **V10 - keyframes and the effect stack**: keyframe lanes and the curve
  editor; transform, crop, blend modes, blur, sharpen, chroma key, masks,
  shadow, vignette; adjustment clips.
  - Host tests for each effect against its scalar reference; the
    keyframe interpolation is held to its formulas.
- **V11 - colour**: wheels, curves, HSL secondaries, `.cube` LUTs, the
  reference frame, and the four scopes drawn by C into surfaces.
  - Host test: a LUT held to its file's values at the lattice and to
    trilinear interpolation between. Each scope is held on a pattern whose
    waveform is known.
- **V12 - audio**: the mixer, pan, rubber band, EQ, compressor, gate,
  loudness normalisation (an EBU R128 meter held to the specification's
  test signals), and ducking.
- **V13 - structure**: nested sequences, multicam with sync by sound
  (cross-correlation held on the Mac on two recordings of one tone burst at
  known offsets), speed ramps and frame blending, render-ahead of a section
  into `Cache/`, and the export queue with AV1, Opus, FLAC, WebM, chapters,
  SRT and stills.
- **V14 - the GPU**: the colour, LUT, key, blur, scale and composite passes
  as Vulkan compute through `kvk.c`.
  - Held to V10-V11's C output on lavapipe in QEMU, then on `anv` on the
    M700, where it is measured.
  - Stabilisation is built here, on the GPU: block motion estimation, a
    smoothed path, then a warp.
- **V15 - hardware codecs** on the M700, if `m700-gpu.md` steps 5 to 8 are
  built: H.264 decode for playback, and VDEnc for proxies and export,
  behind `media.h`, with software as the fallback.

Steps V1 to V7 are the Simple editor, a film a person can finish. Steps V8
to V13 are the Full editor. Each step's suites pass before the next. The
whole gate runs when the Media Kit, the Graphics Kit or the Playback Kit
changes, since others stand on them.

---

## 6. Decisions - agreed

**Agreed on 22 November 2026, every recommendation taken** (Diego, 22 November: "agreed with your recommendations on the video editor"). Each decision below stands as recommended.

1. **The name.** *Kosmos Cut* (recommended): short, a verb, and it says
   what it does, as *Write* does. *Or Montage*, *Or Reel*.
2. **One application with a Simple and a Full view** (recommended), the
   same project in both, chosen in the header. *Or two applications*, an
   iMovie and a Premiere, which means two windows to keep in step and a
   project that has to be moved from one to the other.
3. **One image, `media.elf`, shared with Video** (recommended): the Media
   Kit, the Edit Kit and the Playback Kit linked once for every application
   that plays or edits a film. *Or an image for each application*, which
   duplicates the kits in every image and on the disk.
4. **Media referenced where it is** (recommended), found again by name and
   size if it moves, with *Collect files into the project* later. *Or
   copied into the project at import*, which is safer and doubles the disk
   used by every film.
5. **The H.264 encoder: x264** (recommended). It has the best quality for
   its speed, has assembly for SSE and AVX2 and for NEON, and runs on
   threads. It is GPL, which is fine here (`CLAUDE.md`, *Licence*). *Or
   `minih264e`*, already in the tree for the Record Kit: Baseline profile,
   one thread, and noticeably larger files for the same quality. *Or VDEnc*
   on the M700 only, after `m700-gpu.md`. AV1 export would add SVT-AV1 at
   V13.
6. **Export quality by constant quality, one pass** (recommended), with
   two-pass by bit rate only in the full sheet. *Or a bit rate always*.
7. **CPU first for effects, then Vulkan compute at V14** (recommended), so
   every effect has a C reference before it has a shader. *Or Vulkan from
   V3*, faster on the M700 sooner but with no reference to hold it to, and
   lavapipe under QEMU, which is slow.
8. **`project.json`, through `json.lua`** (recommended): readable and
   checked field by field. *Or a Lua table read as data*, as Flight's
   `Aircraft/*.lua` is: shorter to write, and it needs the same checks.
9. **8-bit colour first** (recommended): the Graphics Kit's surfaces are
   `0xffRRGGBB`, and 10-bit and HDR come with Tier 3. *Or 16-bit float from
   the start*, which doubles the memory of every frame and needs new
   primitives everywhere.
10. **Where it sits in the order.** After `media.md` M6 (Video on the Media
    Kit) and the three designs agreed on 22 November (USB audio, display
    modes, System Monitor), V1 to V7 as one run (recommended). *Or sooner*,
    with V0 done first in any case.

---

## What I could not check

- **Speeds.** Every decode, LUT, encode and scrub time above is an
  estimate. M1's measurement of the Media Kit on the M700 is still owed, and
  V8 and V6 measure the rest.
- **Whether x264 builds for Kosmos's POSIX layer unchanged.** It is plain C
  with nasm and pthreads, as dav1d is, which `w5_build.py` builds. It has
  not been tried.
- **Whether FFmpeg's `mov` muxer's faststart pass works through a `struct
  media_io` that writes through a ring.** It needs a read-back of what was
  written, which `media_io` would have to offer for writing.
- **HEVC.** `roadmap.md` lists HEVC among M6's codecs, but the Media Kit's
  build (`tools/w5_build.py`) has no `hevc` decoder today. iPhone films are
  often HEVC, so V0 should add it.
- **A kit window's frame rate with a lent surface changing 30 times a
  second.** GPU Fractal lends surfaces, but nothing yet changes one every
  frame inside a kit window full of widgets. V4 measures it, and if it does
  not hold, the viewer becomes a direct child window.

---

## Sources

Read on 22 November 2026:

- Adobe, *Premiere Pro: Tools panel and Options panel* -
  <https://helpx.adobe.com/premiere/desktop/get-started/tour-the-workspace/tools-panel-and-options-panel.html>
- Adobe, *Proxy workflow* - <https://helpx.adobe.com/premiere-pro/using/proxy-workflow.html>
- Adobe, *Lumetri Scopes* and *Working with video scopes* -
  <https://helpx.adobe.com/premiere/desktop/correct-color/color-mode-fundamentals/working-with-video-scopes.html>
- Adobe, *Create a multi-camera source sequence* -
  <https://helpx.adobe.com/premiere-pro/using/create-multi-camera-source-sequence.html>
- PremiumBeat, *An in-depth look at the Adobe Premiere Pro editing tools* -
  <https://www.premiumbeat.com/blog/an-in-depth-look-at-the-adobe-premiere-pro-editing-tools/>
- Frame.io, *The Complete Guide to Premiere Pro Proxies* -
  <https://blog.frame.io/2023/10/02/guide-to-premiere-pro-proxies-proxy-workflows/>
- Apple, *iMovie User Guide for Mac* - <https://support.apple.com/guide/imovie/toc>
- Apple, *Change the look of your iMovie project* - <https://support.apple.com/en-us/HT210902>
- Apple, *Final Cut Pro User Guide for Mac* (the magnetic timeline,
  connected clips, storylines, compound clips, roles) -
  <https://support.apple.com/guide/final-cut-pro/toc>
- Blackmagic Design, *DaVinci Resolve* -
  <https://www.blackmagicdesign.com/products/davinciresolve/>
- Kdenlive, *Introduction* (its manual) and *Features* -
  <https://docs.kdenlive.org/en/getting_started/introduction.html>
- Shotcut, *Full List of Features* - <https://shotcut.org/features/>
