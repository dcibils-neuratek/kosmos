<!-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. -->
# Music, the next one: a library player on iTunes's ground, at the file's own resolution

Diego, 22 November 2026: "design and build a new audio player app inspired
in itunes and will hires audio support with flac and mp4, m4a, mp3, wav,
etc", and "we want the best audio player". Just before, about the EDIFIER
M60 on the M700 (UAC1, 16 or 24 bits at 48 or 96 kHz): "so we can play
flac audio file sin full resolution", "in bitrate and all".

This document covers the feature set, what other players do, the
architecture, the busiest path, the steps and the decisions.
`player.html` shows the windows, and `player-architecture.png` (drawn from
`player-architecture.html`) shows what the player stands on. It was written
on 22 November 2026, before any of it is built, and is **for Diego to agree
or change**. Every song, album and artist in the mockup is made up.

---

## In short

- **This is Music's next version, not a second player** (decision 1). The
  design agreed on 14 October (`music.md`, steps M1 to M6) was never built.
  `user/bin/apps/music.lua` is still the VOX pilot of 14 September. This
  design keeps every piece of that one: the window in Kosmos's own look,
  Songs, Albums, Artists, the L/R meter, the equaliser in the audio server
  and the library kit. It adds iTunes's ground on top: playlists and smart
  playlists, Up Next, a column browser, ratings and play counts, a mini
  player, gapless playback and ReplayGain. It also adds the formats and the
  resolution Diego asked for. When it lands, `music.md` is marked as folded
  in here and `music.lua` goes.
- **The application is thin.** Reading and decoding are done by the
  **Media Kit**, which exists and already decodes FLAC, MP3, AAC, Vorbis,
  Opus and WAV from MP4, Ogg, FLAC and WAV files. It grows ALAC and AIFF.
  Sound is played by the **Playback Kit** on its thread in the audio band.
  It grows PCM at any depth and rate, and a queue that joins songs without
  a gap. Mixing and the output are the **audio server's**. It grows the
  hi-res path the roadmap already names (*Hi-res, ASKED 22 November*). The
  player invents no sound path of its own.
- **The resolution is shown honestly, and the server says it, not the
  player.** Only the audio server knows whether anything was mixed in,
  scaled or resampled. So it reports, per stream, the format that reached
  the device and whether every bit got there. The window shows the result:
  **FLAC 96 kHz / 24-bit - bit-perfect**, or **FLAC 96 kHz / 24-bit -
  converted to 48 kHz / 16-bit (HD Audio)**, with the reason.
- **Three pieces, one new and two grown, each with a door, because another application wants
  each one:**
  - **the Library kit** (`library.lua`): songs, albums, artists,
    playlists, ratings and play counts, kept per decision 3. Video wants
    it for films, Tracker's search and the Open window's preview want its
    facts, and Automations wants "the songs I added this week".
  - **the Playback Kit's queue** (`playback.h` grows): a list of files
    decoded ahead on a thread of their own and joined sample for sample.
    Video M6 wants the same door for a film's sound, and Flight's radio
    and a podcast program want it too.
  - **the tag reader, grown** (`tags.lua`): Vorbis comments, FLAC's
    picture block, MP4's `ilst` atoms, iTunes's gapless note and
    ReplayGain's tags. Tracker's Info window and the Open window's preview
    want the same reader.
- **Like Cut, it is an installed application in the `media.elf` image.**
  The Media Kit stands on the POSIX layer, so it cannot be in the system's
  image. Cut's decision 3 (agreed) made `media.elf` the one image for every
  application that plays or edits a film. The player names it too, so there
  is one Media Kit on the disk.

---

## 1. What other players do

Researched on 22 November 2026 from each product's own pages, changelogs and
user forums (see *Sources*). One line each says what people praise. The
features that follow come from that.

- **iTunes (and Apple's Music app after it).** The library is the
  application. It has a column browser (Genre, Artist, Album) over a song
  list sorted by any column. **Smart playlists** are rules over rating,
  play count, last played, date added and genre, and they update live as
  the library changes. iTunes ships some ready-made: *Recently Added*,
  *Top 25 Most Played*, *My Top Rated*. A play counts only when a song
  finishes, which feeds those rules. **Up Next** is a queue you can see and
  reorder. iTunes also had a mini player, Sound Check (loudness evened out)
  and gapless albums. What people miss in it: FLAC (it never had it) and
  any account of what reaches the DAC.
- **foobar2000.** Every format, **gapless**, **full ReplayGain**
  (calculated and applied, per track or per album), an equaliser and
  crossfade as DSP, exclusive output (WASAPI, ASIO), keyboard shortcuts,
  and statistics. It is praised for exactness and speed and criticised for
  its looks.
- **Roon.** **Signal path**: a diagram of every stage between the file and
  the DAC, marked *Lossless* or *Enhanced* or *High quality*. That one
  feature is why audiophiles trust it. It sets the DAC to the file's rate
  and depth. Its album and artist pages are rich, with credits and
  reviews from online services.
- **Audirvana.** Exclusive (*integer*) mode to the DAC, the output rate
  following the file, and the device's own volume used so the bits stay
  untouched. The forum's bit-perfect tests show the results depend on the
  DAC and the depth, which is why a player should *measure* bit-perfect,
  not claim it.
- **Swinsian.** iTunes's library without iTunes: FLAC and every format,
  **folder watching** (new files appear by themselves), smart playlists
  (including "is in playlist" as a rule), and **tag editing on many songs
  at once**.
- **Doppler.** It plays local files: FLAC, MP3, WAV, AAC, M4A. It is
  praised for being simple and pretty. It has *Recently added*, search
  across everything, an album art finder, basic tag editing, and listening
  reports kept on the device only.

**What the praised ones have in common, and so tier 1 below:** a fast
library by songs, albums and artists, with art; search as you type; gapless
albums; every common format; and some honesty about quality. Smart
playlists and play history are what make a library feel alive, and they are
tier 2. Tier 2 also covers the iTunes things people use every day: Up Next,
the mini player and ratings.

---

## 2. What a person can do (the feature set)

### Tier 1: a first player worth using

- **Play every common format**:
  - **FLAC**: 16 and 24 bits, 44.1 to 192 kHz.
  - **ALAC** and **AAC** in **M4A / MP4**.
  - **MP3**, CBR and VBR.
  - **WAV**: 16, 24 and 32-bit integer and 32-bit float.
  - **AIFF**.
  - **Ogg Vorbis** and **Opus**.

  Stereo is the target. A file with more channels is mixed down, and the
  window says so.
- **At the file's own resolution.** A 96/24 FLAC played alone, with the
  volume set to full, reaches the M60 bit for bit. Each song shows its
  format, and the window shows what reached the device:
  - *FLAC 96 kHz / 24-bit · bit-perfect*;
  - *ALAC 44.1 kHz / 16-bit · converted to 48 kHz (the speaker takes 48 or
    96)*;
  - *MP3 320 kbps · decoded to 44.1 kHz / 24-bit*.

  One click opens **the signal path** (Roon's idea, drawn our way). It
  shows the file, decode, stream, mix and device, each stage with its
  format, and marks the stage that changed the bits, if one did.
- **Songs**: a list with number, title, artist, album, time, format and
  quality, sorted by any column. The song playing is marked by moving bars.
  Double-click or Return plays.
- **Albums**: a grid of covers. A cover comes from the file's own picture
  (ID3 `APIC`, FLAC's `PICTURE` block, MP4's `covr`) or from `cover.jpg` or
  `folder.jpg` beside the songs. An album with no picture gets one drawn
  from its name on a colour.
- **An album opened**: the cover large, the year, the number of songs, the
  length, the format (*Hi-Res 96/24* on a lossless album); **Play** and
  **Shuffle**; and its songs in disc and track order. A compilation
  (*Various artists*, `cpil`, `COMPILATION=1`) is one album, not twenty.
- **Artists**: each artist with their albums as covers, by album artist
  where the file names one.
- **Search** as you type, across title, artist, album, album artist,
  composer and genre. Results are grouped as Songs, Albums and Artists.
- **Now playing**, always on top: the cover, the song, artist and album,
  the format badge, shuffle, previous, play/pause, next and repeat (off,
  all, one), a bar you can drag, the time, the volume and the L/R meter.
- **Gapless**: an album recorded without gaps plays without them. MP3's
  encoder delay and padding (LAME's header) and AAC's (iTunes's
  `iTunSMPB`, MP4's edit list) are trimmed. FLAC, ALAC and WAV need
  nothing trimmed. The Playback Kit trims, from what the tag reader read,
  and FFmpeg decodes whole (decision 10). Two songs at different rates or depths cannot join:
  the output changes format between them, and that costs a short silence.
  The window does not pretend otherwise.
- **The library finds itself.** It reads `/Home/Music` and its folders,
  plus any folder you add. New files appear by themselves (the folder's
  pulse, `win:watch_folder`), and files on a USB drive appear while it is
  plugged in.
- **The keyboard**: Space plays and pauses, the arrows move, Return plays,
  Control F searches, Control L shows the song playing, and the media keys
  work system-wide.
- **Open with**: Tracker's double-click on a song plays it in the
  library's context (its album next).

### Tier 2: fully equipped

- **Playlists**: made, renamed, reordered by dragging, and songs dropped in
  from anywhere, Tracker included.
- **Smart playlists**: rules over any field (artist, genre, year, format,
  sample rate, bit depth, rating, plays, last played, date added, is in a
  playlist), with *all* or *any*, a limit (*25 songs, most played*), and
  live updating. Five ship ready-made: **Recently added**, **Most played**,
  **Top rated**, **Hi-Res** (lossless at 88.2 kHz or more, or 24-bit) and
  **Never played**.
- **Up Next**: the queue made visible. *Play next* and *Play later* from a
  right-click, reordered by dragging, and *History* beneath it.
- **Ratings, plays and history**: one to five stars, a play counted when a
  song finishes (as iTunes counts), the last played date and skips. They
  are kept by the library, never written into the file (decision 4).
- **The column browser**: Genre, Artist and Album as three columns over
  the song list, iTunes's way of narrowing ten thousand songs with three
  clicks.
- **The mini player**: a small window with the cover, title, the
  transport and the bar, kept on top if you choose. A **Deskbar
  replicant** gives the same controls in the bar.
- **The equaliser** (`music.md` M4, as agreed): ten bands from 32 Hz to
  16 kHz, ±12 dB, a preamp and presets. It runs in the audio server, per
  stream. While it is on, the badge says *converted - equaliser*.
- **ReplayGain**: per track or per album, read from the file's tags (and
  calculated for files without them, tier 3). Off by default, because it
  is a gain and gain is not bit-perfect. The badge says so when it is on.
- **Crossfade** between songs, 1 to 12 s, off by default and never across
  an album played in order.
- **Info**: a song's tags, its file (path, size, format, codec, rate,
  depth, bit rate, encoder) and its plays. Edits go to the library's copy
  and never into the file (decision 4).
- **Internet radio**: stations as a list, played by `radio.lua` (Flight's,
  which exists), shown in the same now-playing bar.
- **Output**: the device chooser, as Mixer's (`usbaudio.md` U5), reachable
  from the signal path, so the speaker can be changed without leaving the
  player.

### Tier 3: later, written down so they are not forgotten

- ReplayGain calculated (EBU R128, the Edit Kit's meter in `videoedit.md`).
- Tag editing written into the files, if decision 4 is ever changed.
- Lyrics, synchronised (LRC) where a file carries them.
- A visualiser (a spectrum on the Vulkan Kit).
- Shared libraries: another Kosmos machine's music over SMB (the SMB kit
  exists), or a DLNA/UPnP renderer.
- Scrobbling to Last.fm or ListenBrainz (needs the network, so it asks).
- Podcasts (subscriptions are a feed reader, which does not exist).
- CD ripping (there is no optical drive driver).
- DSD, WavPack, APE, Musepack: added to the Media Kit when a file asks.
- Multichannel (5.1) played as 5.1, when a device offers it.

### Left out, on purpose

- **A store, accounts, streaming services, DRM.** iTunes's store and
  Apple Music's catalogue are services Kosmos cannot and need not have.
  The FairPlay `.m4p` files from the old iTunes Store are refused, and the
  window says why.
- **Syncing a phone.** There is no protocol for it here.
- **Upsampling "for quality".** A player that turns 44.1 into 192 is
  inventing samples. The player sends the file's own rate, and the device
  converts only if it must.

---

## 3. What it is made of

**The application is the thinnest layer** (`CLAUDE.md`, *Kits, servers and
drivers supply; applications orchestrate*). Each piece was asked two
questions: does another application want this, and does something already
supply it?

| Piece | Supplied by | New, grows, or as it is |
|---|---|---|
| The window: sidebar, header as the title bar, lists, grids of cards, columns, menus, sheets, drag and drop | the UI kit, `ui.lua` | as it is |
| Covers drawn | `media.cover`'s way: the picture handed to the window manager by name, decoded once there, small | as it is |
| Settings, the window's state | `prefs.lua` | as it is |
| Folders watched | `win:watch_folder` and the folder pulse (`pulses.md`) | as it is |
| Places, USB drives appearing | `places.lua`, `drivelist.lua` | as it is |
| Internet radio | `radio.lua` (Flight's F9) | as it is |
| Media keys system-wide | the window manager's bound keys (`shortcuts.md`) | as it is |
| The Deskbar replicant | replicants, as `clock-replicant.lua` | as it is |
| **Containers and decoders**: FLAC, MP3, AAC, Vorbis, Opus, PCM; MP4/M4A, Ogg, FLAC, WAV | **the Media Kit**, `user/kits/media/media.h`, FFmpeg 9.0.2 built whole (`tools/w5_build.py`) | **grows**: the `alac` decoder, the `aiff` demuxer and its PCM decoders (`pcm_s16be`, `pcm_s24be`), the `pcm_f64le`/`pcm_s24be` stragglers; a sound's **source depth** in `struct media_stream`; **integer samples out** (below); the container's tags passed through (`AVFormatContext.metadata`) for formats `tags.lua` does not read |
| **Tags and art**: ID3v2/v1 and WAV `LIST` today | **the tag reader**, `tags.lua` | **grows**: Vorbis comments (FLAC, Ogg), FLAC's `PICTURE` block, MP4's `moov/udta/meta/ilst` (`©nam ©ART aART ©alb trkn disk ©day ©gen ©wrt cpil covr`), iTunes's `iTunSMPB`, LAME's delay/padding, ReplayGain's tags, AIFF's `ID3 ` chunk. Lua, as now: a file's header is read once, and the reader cannot read past what it was given |
| **The library**: songs, albums, artists, playlists, smart rules, ratings, plays, history | **the Library kit**, `user/lib/library.lua` (`music.md` M2's, grown) | **new** - Video's films, Tracker's search and Info, the Open window's preview and Automations want the same facts |
| **Playing a queue of files**: decode ahead, join gaplessly, trim encoder delay, crossfade, ReplayGain's scale, the position in the right song | **the Playback Kit**, `playback.h`: a queue door on a feeder thread (NORMAL band) in front of the thread that exists (AUDIO band) | **grows** - Video M6 needs the same door for a film's sound, and so do the radio and anything that plays a list |
| **PCM at any depth and rate** into the stream's ring | **the Playback Kit**'s WAV kind, widened past 8 and 16 bits | **grows** |
| Converting rate and depth when the device must | `pcmconvert.h`, inside the server and the kit | **grows**: 24 and 32 bits in and out, and a better resampler (decision 5) |
| **The stream at its own format**; the mix in 32 bits at the output's rate; the output following the highest-resolution stream; **bit-perfect reported** | **the audio server**, `audioproto.h`, `audio.c` | **grows** - the roadmap's *Hi-res* entry, written into `usbaudio.md` beside U7. Every program gets it, not only the player |
| The equaliser, the L/R meter | **the audio server**, per stream (`music.md` M3, M4) | **new**, as agreed on 14 October |
| The device: M60 at 96/24, QEMU's speaker at 48/16, HD Audio | `xhci.c` and `uac_decode.c` (`usbaudio.md` U3, U7), the HAL's HD Audio | grows with U7 |
| The hardware volume of a USB speaker | `uac_decode.c`'s feature unit, `xhci.c`'s class request | **new**, small - only if decision 6 chooses it; Mixer wants it too |
| Files read | the Storage Kit and the POSIX layer inside the image (`w3.md`) | as it is |

**Does something already supply it? Four near misses, so none of them is
copied:**

- **`media.lua`** is today's Music engine. It handles MP3 and WAV, the
  Playback Kit's Lua door, and covers and tags. Its *player* is replaced by
  the queue door. Its `tags` and `cover` move into `tags.lua`, which they
  already call. Nothing in it is kept beside the new door. `videoedit.md`
  already notes that it is renamed with M6.
- **The vendored FFmpeg closure** (`user/kits/ffmpeg/`, H.264 and AAC
  only) is Video's until `media.md` M6. The player never uses it. It uses
  the Media Kit, as Video will.
- **Decoding FLAC on the Playback Kit's own thread**, as it decodes MP3 and
  AAC today, would mean a second FLAC door beside the Media Kit's, which
  the premise forbids. So the Media Kit decodes, on the feeder thread, and
  the Playback Kit carries PCM (*The busiest path* says why that is
  cheap).
- **SQLite** is in the tree (W5c, inside JetSki's image) and could hold the
  library. Decision 3 sets it against the roadmap's word, *BeOS's queries,
  not a database beside the files*, and recommends attributes.

### The doors

```lua
-- The Library kit: facts, never samples.
local library = use("/Kosmos/Libraries/library.lua")
local lib = library.open{ folders = { "/Home/Music" } }
lib:refresh(on_progress)        -- reads only what is new or changed
lib:songs{ sort = "artist", where = rule }   -- rows, a page at a time
lib:albums()  lib:artists()  lib:album(id)
lib:search("night fe")          -- { songs, albums, artists }
lib:playlists()  lib:playlist(id)  lib:smart(rule)
lib:played(song)  lib:skipped(song)  lib:rate(song, 4)
lib:watch(on_change)            -- a folder's pulse becomes a refresh

-- The Playback Kit's queue, from Lua (the door also exists in C).
local playback = use("media.elf").playback
local q = playback.queue{ gapless = true, crossfade_ms = 0,
                          replaygain = "off" }      -- "track", "album"
q:set({ path1, path2, ... }, 1)  -- the queue, and where to start
q:insert(at, path)  q:remove(at) -- Up Next edits; never interrupt the song playing
q:play()  q:pause()  q:seek(us)  q:next()  q:previous()
q:state(t)  -- which song, its position (us, from frames heard), the format
            -- the stream runs at, frames dry, why it stopped
q:close()
```

`audioproto.h` grows (the server's half, for `usbaudio.md`'s new step):

- `AUDIO_OP_OPEN` takes a format: `rate`, `bits` (16, 24 or 32;
  integer), `channels`. 0 means the device's default, as today, so every
  program that opens a stream now does not change.
- `struct audio_stream_info` gains: the stream's format, `peak_left` and
  `peak_right` (M3), and **`exact`**: 1 while every sample this stream
  wrote reached the device unchanged in the last period, with a reason code
  when not (*gain*, *master*, *equaliser*, *mixed with another*,
  *resampled*, *narrowed*, *mixed down*).
- The reply's device fields gain the output's running format.

### Where the code lives, and which process

- **An installed application** in Cut's shape: `user/installed/Music/`
  holds its Lua (`music.lua` and the window's parts), and names its image
  with `-- kosmos: image media.elf` and `-- kosmos: needs audio`. It shows
  up in `/Kosmos/Apps` as Music, as JetSki does. **Until `media.elf`
  exists** (Cut's V0 or Video's M6, whichever comes first) the player
  cannot be built, and step P0 says so.
- **One process.** The window is Lua on the main thread. The feeder is a C
  thread at NORMAL: it reads the files, demuxes, decodes, trims and joins.
  The Playback Kit's thread is in the AUDIO band. The library's refresh
  runs on the main thread a slice at a time, so the window never stops for
  it. It is header parsing, a few files per frame.
- **The library's facts are reached the same way from any process.**
  Tracker and the Open window do not link `media.elf`. They read the same
  attributes (decision 3), which is why the facts live on the files and
  not in the player's memory.

### C or Lua

- **Lua**: the window, the library and its rules, smart playlists, search
  (a substring scan over ten thousand rows in memory is a few
  milliseconds), the queue's order, and the tag reader. These are
  structure and tables. A bug in them is a stack trace, and C would buy
  about 2% (`CLAUDE.md`, *Language split*).
- **C**: everything per sample: demuxing, decoding, trimming, joining,
  crossfade, ReplayGain's scale with dither, conversion, the mix, the
  equaliser and the meter. Loops over samples use GCC's vector types, with
  the scalar loop kept as the reference a host test holds them to.

### What travels in a region and what in a message

(`CLAUDE.md`, *Control by message, data by shared memory*.)

- **Compressed bytes** are read by the feeder into its own memory, never a
  Lua string.
- **Decoded PCM** goes from the feeder into the Playback Kit's input ring:
  one writer and one reader, indices only. It is **sized in seconds, not
  bytes**. Eight seconds at the stream's format is 6 MB at 96/24 stereo
  (stored as 32-bit) and 12 MB at 192/24. Today's 1 MB is 1.4 seconds of
  96/24, which is too little for a window that may be away.
- **Periods** go from the Playback Kit's thread into the stream's ring, a
  region shared with the audio server, as now. Then they go from the server
  into the USB ring (`usbaudioring.h`) and from there into `xhci`'s
  transfer descriptors (TDs).
- **Track boundaries** travel *in* the input ring as a record between two
  songs' samples (`{ bytes = 0, flags = BOUNDARY, song }`). So the song the
  window shows changes at the sample it changes, not when a message
  arrives.
- **Commands** (play, pause, seek, queue edits) go from the window to the
  feeder in a ring of fixed records, as the Playback Kit's commands do
  today.
- **What is heard** goes from the thread to the window: a copy under a
  sequence number, as now. It carries the song, frames heard and the
  stream's format, and the server's `exact` comes back in the
  `AUDIO_OP_STREAMS` reply the window already asks for, ten times a second.
- **The library's facts** are attributes on each file, or a store
  (decision 3). Reading them is a message to `/Home` (`query`, `getattr`),
  once per file at refresh.

---

## 4. The busiest path: a 96 kHz / 24-bit FLAC from the disk to the M60

**Figures below that are not measured are labelled as estimates.** QEMU's
times are not speeds (`CLAUDE.md`).

```
  /Home (diskfs, C)          media.elf: the player's process                   audio (C, AUDIO band)        xhci (C)          M60
  ───────────────            ─────────────────────────────────────────         ─────────────────────        ────────          ───
  read 1 MB pieces ──► feeder thread (NORMAL)                                   stream ring ─► mix in 32 bits ─► USB ring ─► iso TDs ─► 96/24
                        demux (mov/flac) ─► decode (FFmpeg flac, s32)             (gain 256, nothing          (24-bit,      a ms at a
                        ─► trim ─► [join] ─► input ring (8 s, s32) ──► Playback    else playing:              96 kHz,       time
                                                                       thread      exact = 1)                 9 pages)
                                                                       (AUDIO): a period into the ring
```

1. **The disk.** A 96/24 FLAC is about 3,000 to 4,000 kb/s, roughly 450 KB a
   second. The feeder reads 1 MB pieces, one every two seconds or so.
   `diskfs` measured 514 MB/s sequential (`diskfs.md` 3b), so this is
   nothing.
2. **Decode.** FFmpeg's FLAC decoder at 96/24 stereo: 192,000 samples a
   second. *Estimate:* well under 2% of one M700 core (FLAC is designed to
   decode cheaply, and FFmpeg's has assembly for its LPC). Measured in P1.
3. **Integer samples, not float.** The Media Kit hands sound back as float
   today. Float holds a 24-bit sample exactly (its mantissa is 24 bits), so
   16 and 24-bit sources would survive the round trip. A 32-bit integer
   WAV would not, and the conversion has to be checked bit for bit anyway.
   So the queue asks the Media Kit for the decoder's own integer format
   (`s16`, `s32` with the 24 bits at the top) and float only for lossy
   codecs, whose output *is* float. P1's host test holds this: FFmpeg's
   `-f md5` of the decoded samples, equal to ours.
4. **Into the input ring**, 8 seconds ahead, as `int32` stereo (768 KB a
   second). The window can be away for eight seconds and nothing is heard
   missing.
5. **The Playback Kit's thread** (AUDIO band) copies a period at a time
   into the stream's ring. The stream was opened at 96 kHz / 24-bit, so
   there is no conversion. *Estimate:* a copy of about 2 KB a millisecond.
6. **The audio server** mixes in 32 bits at the output's rate. With one
   stream at gain 256, master at 256, no equaliser and nothing else
   playing, the mix is the stream's samples unchanged, and `exact` stays 1.
   It writes the M60's 24-bit frames (3 bytes each) into the USB ring.
7. **`xhci`** sends 96 frames a millisecond, 576 bytes, per isochronous
   TD, as U2 does at 48 kHz today. The M60's setting 1.2 takes 96/24.

**Where the time goes:** decode and the server's mix. Each is a fraction of
a core at this rate (estimates until P1 and P6 measure them). **Where the
risk is** the moment the output changes format between two songs at
different rates. The USB ring is not a risk at 96/24: its 9 pages hold
6,133 frames of 6 bytes, about 64 ms, well over `USB_AUDIO_KEEP_MS`'s 23.
At 192/24 they hold about 32 ms, still enough.

**The window**, ten times a second while playing, repaints only the meter,
the bar and the time, as damaged strips. **Opening a library** of ten
thousand songs: the attributes' query, or the store, read in one go, and
the grid drawn from it. Covers are decoded as they come into view, a few
at a time, small, and kept while the window lives.

---

## 5. Steps

Each step ends in something checked. The gate runs on x86-64 QEMU (Diego's
rule since 8 October). The Mac runs host tests of pure C, and of Lua on the
host's interpreter. Test files are **made at the test's start** by the
Mac's FFmpeg and `flac` from tones and noise: a 997 Hz sine, a sweep, and
pseudo-random noise with a known seed. They come in every format, at 44.1,
48, 96 and 192 kHz, and at 16 and 24 bits. No one's music goes into the
repository. **Every check has a control that must fail.**

- **P0 - before any code**: `media.elf` exists (Cut's V0 or `media.md` M6),
  and the audio server's hi-res step is written into `usbaudio.md` and
  agreed (*What this asks of the audio server*, below).
- **P1 - the Media Kit for music.** `alac`, `aiff` and the big-endian PCM
  decoders built; the source depth in `struct media_stream`; integer
  samples out.
  - Host test, `tools/test_mediasound.c`: every generated file decoded and
    its samples' MD5 held to `ffmpeg -f md5` with the same sample format.
    For FLAC, it is also held to the `STREAMINFO` MD5 the file carries,
    which is the encoder's own word on what the samples are.
  - Control: converting through float for a 32-bit integer WAV must fail
    the MD5.
- **P2 - the tag reader grown.** Vorbis comments, FLAC's `PICTURE`, MP4's
  `ilst` and `covr`, `iTunSMPB`, LAME's header, ReplayGain, AIFF's `ID3 `.
  - Host test, `tools/test_tags.lua` (exists, grows): tags built by hand
    and from files the Mac's FFmpeg tagged.
  - Control: an atom whose size points past its parent must be refused,
    not followed.
- **P3 - the Playback Kit: PCM of any depth, the input ring in seconds,
  the queue and its joins.** On the Mac first, as `test_playback.c` holds
  the kit today.
  - Host test: two songs from one album, joined, are **sample for sample**
    the decode of the uncut original. A gapless MP3 pair has LAME's delay
    and padding trimmed, and so does an AAC pair with `iTunSMPB`. The
    boundary record lands at the exact frame. A seek into the second song
    lands at its frame.
  - Control: joining without trimming must fail the comparison.
- **P4 - the Library kit.** `library.lua` on decision 3's store: refresh,
  albums (compilations grouped), artists, search and smart rules.
  - Host test, `tools/test_library.lua`, on a tree of generated files: the
    expected albums and counts; a file changed is read again and one
    unchanged is not (reads counted); every smart rule against a table of
    expected members.
  - Control: a refresh that ignores modification times must read every
    file and fail the count.
- **P5 - the window.** Songs, Albums, an album opened, Artists, search,
  now playing and the badge, as `player.html` draws them; the installed
  application in `media.elf`; Tracker's *Open with*.
  - Gate suite `x86-player`: a library of generated files on the QEMU disk,
    the window opened, an album played. QEMU's `-audiodev wav` holds the
    997 Hz tone at its frequency with no gap, `run_film.py`'s way. The
    window's text is read for the badge.
  - Control: the suite run with the sound muted must fail its tone check.
- **P6 - bit-perfect, measured end to end** (after the server's hi-res
  step and `usbaudio.md` U3).
  - Gate suite `x86-player-exact`: **a 48 kHz / 16-bit FLAC of noise**,
    played through QEMU's USB speaker (48/16, so the format matches). The
    WAV QEMU writes must contain the decoded samples **bit for bit**, found
    by aligning the first 4,096 samples, and the badge must say
    *bit-perfect*.
  - Controls, three of them, each of which must fail bit-perfect and must
    make the badge say *converted* with the right reason: volume at 99%; a
    beep from another program mixed in; a 44.1 kHz file (resampled).
  - On the M700, the M60 at 96/24: the server's `exact` read over Telnet
    while a 96/24 file plays, and by ear. The M60 cannot be read back, so
    the bits beyond the server are not proved (*What I could not check*).
- **P7 - gapless across the device.** Two songs at different rates in a
  row. QEMU's WAV shows the first ending and the second starting, with a
  silence no longer than one format change, measured and written down.
  Two songs at the same rate show none.
  - Control: inserting a period of silence at a same-format join must fail.
- **P8 - playlists, smart playlists, Up Next, ratings, plays and history,
  the column browser.** `test_library.lua` grows. `x86-player` adds a
  playlist made, a song rated and a play counted only at the song's end.
  - Control: a play counted at a skip must fail.
- **P9 - the mini player, the replicant, the media keys.** The display
  harness opens both and presses the keys.
- **P10 - the equaliser and the L/R meter** (`music.md` M3, M4, in the
  server). A tone through a band cut by 12 dB, measured in QEMU's WAV.
  - Control: the band set and not applied must fail.
- **P11 - ReplayGain and crossfade.** Host tests: ReplayGain's scale with
  TPDF dither, held to a reference within one LSB; a crossfade's curve held
  to its formula.
  - Control: ReplayGain *off* must leave the samples bit-identical, so
    applying it unasked fails.
- **P12 - Info, internet radio, the output chooser in the signal path.**

Each step's suites pass before the next. **The whole gate runs after P3 and
P6**, since the Playback Kit and the audio server are shared by every
program that makes a sound.

---

## 6. What this asks of the audio server and the Playback Kit

**Of the audio server** (the roadmap's *Hi-res* entry, to be written into
`usbaudio.md` as a step beside U7 and agreed. The player adds nothing to
it but the report):

1. `AUDIO_OP_OPEN` with a format: rate, depth (16, 24 or 32-bit integer),
   channels. 0 means as today.
2. The mix in 32 bits at the output's rate (today it is 16-bit at 44.1).
3. The output's format follows the highest-resolution stream that is
   playing, within what the device offers (the M60: 48 or 96 kHz, 16 or 24
   bits; 192 is converted to 96). Other streams are converted into it.
4. Unity is unity. With gain 256 and master 256, a stream's samples pass
   through the mix unchanged. Narrowing (32 to 24 bits for the device, or
   24 to 16) is exact when the low bits are zero, and is dithered when
   they are not.
5. **`exact` per stream**, with its reason, in `AUDIO_OP_STREAMS`. The
   server is the only process that can know, so it is the only one that
   says.
6. The USB ring checked at the new rates. Its 9 pages hold about 64 ms of
   96/24 and 32 ms of 192/24, so it holds `USB_AUDIO_KEEP_MS` as it is.
7. A better resampler (decision 5), since a hi-res player will hand it
   44.1 kHz songs for a speaker that takes only 48 or 96.
8. A format change between two streams, or two songs, done as one step:
   drain, set the new alternate setting, start. The time it takes is
   measured and reported, so P7 can hold it.

**Of the Playback Kit** (a step of `playback.md`, grown):

1. PCM of any depth and rate as its WAV kind: 16, 24 or 32-bit integer and
   32-bit float, 1 to 8 channels (more than 2 mixed down, and said).
2. The input ring sized in seconds of the stream's format.
3. The stream opened at the source's format, through the new
   `AUDIO_OP_OPEN`.
4. **The queue door** (`playback_queue_*` in C, `playback.queue` in Lua),
   with a feeder thread at NORMAL on the Media Kit. It decodes ahead,
   trims encoder delay and padding, joins same-format songs with no gap,
   writes boundary records, crossfades and applies ReplayGain with dither,
   but only when asked.
5. A format change at a boundary handled by reopening the stream at the
   new format. That is the one place a gap is allowed, and it is counted
   in what it heard.

---

## 7. Decisions - agreed

**Agreed on 22 November 2026, every recommendation taken** (Diego, 22
November: "agreed with your recommendations, go ahead"). Each decision below stands as recommended.

1. **What this is to Music.**
   - *Music's next version* (recommended). It keeps the name *Music* and
     folds in `music.md` (M1 to M6, agreed and not built). `music.lua`,
     the VOX pilot, goes when P5 lands. One player, and the 14 October
     design's look, meter, equaliser and library survive inside this one.
   - *Or a second application* beside Music (a new name, and Music kept
     for simple listening). That would be two players over the same kits,
     with two windows to keep in step.
   - *Or build `music.md` M1 to M6 first* and grow it into this. That
     means the same pieces, built twice in the window.
2. **The name.**
   - *Music* (recommended): it is the system's music player, and Kosmos
     names applications for what they are (Write, Mail, Maps).
   - *Or a name of its own*, such as *Kosmos Sound* or *Listen*. (*Listen*
     is already the name of a program in the image, `testing.md` 18.892,
     so it would need renaming.) "iTunes" and its icon are Apple's and are
     not used.
3. **Where the library keeps its facts.**
   - *Attributes on the files, found by query* (recommended). Tags read
     once become BeOS-style attributes on the file (`audio:title`,
     `audio:artist`, `audio:album`, `audio:rate`, `audio:bits`,
     `audio:plays`, `audio:rating`, ...). The file's bytes are never
     touched, which keeps 14 September's word: tags are read, never
     written. A query over `/Home/Music` finds them, Tracker and the Open
     window read the same facts, and playlists are small files in
     `/Home/Music/Playlists`. This is the roadmap's *Music and photos as
     libraries: BeOS's queries, not a database beside the files*. Its
     cost: a query scans, and diskfs's 3b numbers (2,000 files by name in
     6 ms; 200 answers with attributes in 12 to 15 ms) suggest about half
     a second to a second for ten thousand songs under QEMU. So the kit
     keeps a snapshot of the last answer, draws from it at once and
     reconciles behind. A drive without attributes (a FAT stick) is read
     each time it is plugged in, and nothing is kept for it.
   - *Or one index file*, `/Home/Music/.library`, as `music.md` M2 agreed.
     It is fast and simple, but only the player can read it.
   - *Or SQLite*, which is in the tree. It is fast for large libraries, is
     a database beside the files, and is reachable only from images that
     link it.
4. **Edits to tags, ratings and plays.**
   - *Kept in the library (attributes or the store), never written into
     the file* (recommended), as decided on 14 September.
   - *Or written into the file*, as Swinsian and iTunes do. The edit then
     travels with the file to other computers, and Kosmos would gain a tag
     writer for four formats, which is a lot to get right. Tier 3 if ever.
5. **The resampler, for when the device cannot take the file's rate.**
   - *A windowed-sinc polyphase resampler in `pcmconvert.h`* (recommended),
     the one copy that the server and the Playback Kit use. It is
     vectorised and held by a host test of its passband, stopband and
     THD+N. Linear interpolation, today's, is audibly worse on bright
     material at 44.1-to-48. `pcmconvert.h` itself says it was chosen for
     a laptop speaker, and this player is not for one.
   - *Or FFmpeg's `swresample`*, which is built already, but only inside
     `media.elf`, so the server could not use it, and there would be two
     resamplers.
   - *Or keep linear interpolation* and rely on the output following the
     file, so that conversion is rare.
6. **Volume and bit-perfect.**
   - *The software volume, with the badge honest* (recommended for now).
     Below full volume the badge says *converted - volume*. It is simple
     and it is true.
   - *Or the speaker's own volume* (Audirvana's way): the player's slider
     moves the M60's UAC feature unit, so the bits stay exact at any
     volume. It needs a class request in `xhci.c` and a feature unit
     found by `uac_decode.c`. Mixer wants it too, so it is a good
     `usbaudio.md` step after U8.
7. **When another sound plays over a hi-res song.**
   - *Shared, and said* (recommended): a notification's beep is mixed in,
     and for that moment the badge says *converted - mixed*. Nothing is
     ever silenced without being asked.
   - *Or an exclusive mode* that the player can ask for, where other
     programs' sound is held while it plays. That is audiophile-correct,
     and surprising the first time a call rings silently.
8. **Formats in tier 1.**
   - *FLAC, ALAC, AAC, MP3, WAV, AIFF, Vorbis, Opus* (recommended). All are
     FFmpeg decoders, and two are not yet built.
   - *Or add WavPack, APE and DSD now.* DSD in particular needs DoP or a
     DSD-capable device, which the M60 is not.
9. **Where it sits in the order.** After `usbaudio.md` U3 and U4 and the
   server's hi-res step, and once `media.elf` exists. Then P1 to P7 as one
   run, which is the part Diego asked for (FLAC at full resolution through
   the M60), and P8 to P12 after (recommended). *Or the window first*, P5
   on today's 16-bit path, with hi-res arriving under it later.
10. **Who trims a song's priming and padding** - decided 23 November 2026,
    when P1 and P3 were built side by side and found that both could
    (Diego: "go with the kit doing the trimming").
    - *The Playback Kit* (chosen). The Media Kit decodes a song whole
      (`MEDIA_UNTRIMMED`, FFmpeg's `AV_CODEC_FLAG2_SKIP_MANUAL`), and the
      player hands the kit the delay and length `tags.lua` read - LAME's
      delay plus the MP3 decoder's 529, or `iTunSMPB`'s priming as written.
      The kit trims with two index comparisons, marks each song at its first
      kept frame, and checks a stated length against what arrived: a length
      far short of the stream is not believed, and the song plays to its
      end (`untrusted`). That matters because **this Mac's FFmpeg writes a
      wrong LAME header into an MP3 that carries a picture** - two frames,
      padding 576 - and FFmpeg's own trim believes it, cutting a one-second
      song to 26 ms.
    - *Or FFmpeg*, its default: the player sends no delay or length. Less
      to build, and nothing checks what FFmpeg trusted.

---

## What I could not check

- **The M60's bits beyond the server.** Kosmos can prove what it writes
  into the USB ring and what QEMU's speaker receives (P6). It cannot read
  back what the M60's DAC got. Proving that needs a USB analyser or a
  digital output looped back.
- **Whether the M60 has a UAC feature unit for volume** (decision 6). Its
  descriptors were read in U1 (`testing.md` 18.996), and I did not go
  through them for a feature unit.
- **Decode speeds.** Every decode time above is an estimate. The Media
  Kit's speed on the M700 is still owed by `media.md` M1.
- **Whether the Media Kit's sound is converted to float by `swresample` or
  by its own loop**, and whether either is exact for 24-bit sources. P1's
  MD5 test decides it. Integer out is the design either way.
- **How long the M60 takes to change its alternate setting** (48 to 96
  kHz) between two songs. It sets the gap P7 allows.
- **A query's time over ten thousand songs with attributes** (decision
  3). It is extrapolated from 3b's numbers for 200 answers.
- **`media.elf`'s size with ALAC and AIFF added.** It is expected to be
  small: both are a few thousand lines of FFmpeg.
- **FairPlay files** are recognised by their `drms` sample entry. That is
  from memory of the format, not checked against a file.

---

## Sources

- foobar2000, overview and components: https://www.foobar2000.org/ ,
  https://foobar2000.org/components/author/Case
- Roon, signal path and exclusive mode (Roon Community):
  https://community.roonlabs.com/t/raat-signal-path-in-roon-1-4-answered/35663 ,
  https://community.roonlabs.com/t/bit-perfect-playback-problem-resolved-roon-is-bit-perfect/27833 ,
  https://roon.app/sound-quality
- Audirvana compared, bit-perfect tests:
  https://community.roonlabs.com/t/audirvana-vs-roon/61175?page=6 ,
  https://community.roonlabs.com/t/roon-v-audirvana/185273?page=2
- iTunes smart playlists, play counts: https://help.apple.com/itunes/mac/12.4/en.lproj/itns3001.html ,
  https://www.oreilly.com/library/view/ipod-the-missing/9780596155834/ch06s10.html ,
  https://www.macworld.com/article/1141930/itunes_playcount.html
- Swinsian: https://swinsian.com , https://swinsian.com/support/changelog ,
  https://swinsian.com/blog
- Doppler: https://apps.apple.com/app/id1468459747 ,
  https://thesweetsetup.com/a-first-look-at-the-doppler-2-music-app/
