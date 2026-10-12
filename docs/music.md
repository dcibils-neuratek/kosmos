# Music

Kosmos's music player, redesigned in the system's own look. Diego, 11
October 2026: "lets redesign the music app with a more native music app
with the ui style of kosmos", "it needs to support mp3, wav, flac", "album
view as well", "song view as now", "a small uv meter", "equializer",
"mockup first", "make it look nice and native for kosmos". Drawn the same
evening in `docs/music.html` and agreed: "music mockup is perfect".

This document is the feature set and the architecture; `music.html` is the
window and `music-architecture.png` the picture of what it stands on. It
replaces the plan of 14 September, which drew Music after VOX in a palette
of its own (`roadmap.md`, Music, as VOX is).

## What a person can do

- **Play MP3, WAV and FLAC.** A FLAC of 16 or 24 bits, at 44.1, 48 or 96
  kHz; 24-bit is dithered to the 16 the sound path carries until it carries
  more, and the format and quality are shown beside what is playing -
  *FLAC · 24-bit · 96 kHz*, *MP3 · 320 kbps*, *WAV · 16-bit · 44.1 kHz*.
- **Songs**, as Music has them now: a list - number, title, artist, album,
  format, time - sorted by any column, the song playing marked by three
  moving bars where its number was. Double-click or Return plays.
- **Albums**: every album as its cover in a grid, from the file's own
  picture (an MP3's tag, a FLAC's picture block) or a `cover.jpg` or
  `folder.jpg` beside its songs; an album with none gets one drawn from its
  name's letters on a colour. The album playing is outlined.
- **An album opened**: the cover large, its year, how many songs, how long,
  its format; **Play** and **Shuffle**; its songs in order. ‹ Albums, or
  Escape, goes back.
- **Artists**: the same list, grouped by artist (a step after the others).
- **Search** across title, artist and album as you type.
- **What is playing**, always on top as now: the cover, the song, artist and
  album, its format; shuffle, previous, play and pause, next, repeat; a bar
  you can drag; the time.
- **A meter**: left and right as two rows of twenty lamps - green to −12
  dB, yellow to −3, red above - the highest lamp of the last second held.
- **The volume**, and **an equaliser**: ten bands an octave apart, 32 Hz to
  16 kHz, each ±12 dB, a preamp, on or off; presets - Flat, Bass, Treble,
  Vocal, Rock, Classical, Late night - and your own, kept by name. It shapes
  what Music plays and nothing else.
- **Where the music comes from**: `/Home/Music` and its folders, and
  Downloads, as the sidebar's Places; another folder by Open.
- **From the keyboard**: Space plays and pauses, the arrows move, Return
  plays, Control F searches, Control E opens the equaliser.

Left for later, as the 14 September plan had them: playlists, a mini
player, the Deskbar replicant, and editing a file's tags.

## What it is made of

**The application is the thinnest layer** (`CLAUDE.md`, kits supply,
applications orchestrate): Music arranges what the system supplies into a
window, and every piece another application could want is a kit or a
server, with one door.

| Piece | Supplied by | New or as it is |
|---|---|---|
| The window: sidebar, header as title bar, list, grid of cards, menus | the UI kit, `ui.lua` | as it is |
| Reading, decoding, feeding, the clock, seeking | `media.lua` | as it is, with FLAC behind its door |
| MP3 | the MP3 kit (`/Kosmos/Kits/mp3`, minimp3) | as it is |
| WAV | `media.lua`'s reader | as it is |
| **FLAC** | **a FLAC kit**, `/Kosmos/Kits/flac`, FFmpeg's decoder as the AAC kit is (`user/kits/ffmpeg/`) | **new** |
| Tags and covers | the tag reader, `tags.lua` | **grows**: FLAC's Vorbis comments and `METADATA_BLOCK_PICTURE` |
| A cover drawn | `media.cover` - the picture handed to the window manager by name, decoded once there | as it is |
| **The library**: which songs, their tags, grouped into albums | **a library kit**, `library.lua`: a folder scanned, tags read once and kept beside it in `/Home/Music/.library`, refreshed by what changed | **new** - Video and the Open window's preview want the same index |
| **The equaliser** | **the audio server**, per stream: ten biquad peaking filters a channel and a preamp, in C, set by `AUDIO_OP_SET` | **new** - Video, Groove and the Mixer want one; every application gets it by asking |
| **The meter, left and right** | **the audio server**, per stream: a peak a channel, where it keeps one for both now | **new field** |
| Presets and settings | the settings kit, `prefs.lua`, as `music` | as it is |

**C or Lua.** Decoding and the equaliser are loops over samples and are C
- the FLAC kit and the audio server. The library is paths and tables and is
Lua. The window is Lua, drawing through the kit, whose pixels are C.

**What travels in a region and what in a message** (`CLAUDE.md`, control
by message, data by shared memory):

- **The samples** go from `media.lua`'s decoder into the stream's ring, a
  region, and from the ring through the server's mix to the device. Never a
  message.
- **The equaliser's settings** are a message, `AUDIO_OP_SET`, sent when a
  slider moves: ten gains and a preamp in the declared shape, with -128
  meaning "leave this one", as `gain` has -1.
- **The meter** is read in the `AUDIO_OP_STREAMS` reply Music already asks
  for, ten times a second: a stream's `peak_left` and `peak_right`.
- **A cover** goes once, as a region, to the window manager, and is drawn
  by name after.

**The equaliser in the server, not in Music.** Music could filter its own
samples before they go into the ring, and Video and Groove would then each
need a copy - the thing the premise forbids. In the server it is one copy,
applied per stream in the mixing pass where `gain` and `balance` already
are, so a stream with no equaliser costs a comparison. Ten biquads a
channel at 48 kHz is under a million multiply-adds a second, in a loop the
compiler vectorises, inside a 5.8 ms period. The coefficients are worked
out when a setting arrives, not in the mix. Groove's sound band is not
slowed: a stream with its equaliser off takes the path it takes now.

**The library kit, not Music's own scan.** Today Music lists a folder and
reads tags as songs are chosen. Albums need every song's album tag before
the grid can be drawn, and a thousand songs is a thousand small reads, so
the result is kept: one file of what each song said, against its size and
when it was changed, read in one go next time, with only new or changed
files read again.

## The busiest path

**A song playing**, every period: `media.lua`'s feed decodes - minimp3, the
FLAC kit, or a WAV's samples - into the ring, as far ahead as the ring
allows (C, no Lua string per period since the shared-ring refactor); the
server mixes the stream, applies its equaliser and gain, measures its peak
a channel, and hands the period to the device. Where the time goes, on the
M700: decoding a FLAC frame is about the cost of an MP3 one; the equaliser
under a tenth of a millisecond a period; the rest is the mix as it is now.

**The window**, ten times a second while playing: the meter's two rows and
the bar's position repainted - a damaged strip, not the window - from one
`AUDIO_OP_STREAMS` reply. The list and the grid paint only when they change
or scroll.

**Opening the library**: the kept index read in one go and the grid drawn;
covers decoded as they come into view, a few at a time, small, and kept
while the window lives.

## Steps

Each its own test, in this order:

1. **M1 - the window**: Music in the kit's look, Songs and what is playing
   as now, the sidebar; nothing new underneath. `run_media.py`'s checks kept.
2. **M2 - the library kit and Albums**: the index kept beside the music, the
   grid of covers, an album opened, drawn covers for albums with none.
3. **M3 - the meter**: `peak_left` and `peak_right` in the server's reply,
   the two rows of lamps.
4. **M4 - the equaliser**: biquads in the server per stream, the panel,
   presets kept. Tested by a tone through a band cut and boosted, measured in
   what QEMU's sound card wrote.
5. **M5 - FLAC**: the FLAC kit, its tags and picture, 24-bit dithered.
6. **M6 - Artists and search.**
