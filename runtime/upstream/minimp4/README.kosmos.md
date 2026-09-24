# minimp4, vendored

Upstream: <https://github.com/lieff/minimp4>, commit
`5a212a18dba7dca09543bbc7d65619274fd2931a` (27 July 2026), downloaded on
24 September 2026 as the tarball GitHub serves for that commit.
Licence:  CC0 1.0 Universal (public domain). See `LICENSE`, and the notice at
          the top of `minimp4.h`.

Two files, taken byte for byte: `minimp4.h` and `LICENSE`. **Unmodified**.

    1fc8d29b8c29dbeead9710bd9e95f6aab4d2fec48a55c2cc54a59f0338190bf2  minimp4.h
    6a1ee543e5282cd9061881edf462e6fdab181f328da71fc2c9a6950a80e94d01  LICENSE

## What it is for

The Record Kit's MP4 writer (`user/kits/record`, `roadmap.md` 6d 8f): the
encoder's NAL units into an ISO base media file, its index after its frames,
written through a callback - here into a region the Camera app made, which
is written to the disk whole when the recording stops.

## How it is built, as build steps rather than edits

- **Its `malloc`, `realloc` and `free` are the recording's arena**
  (`record_core.c`): the three names are defined before the header is
  included, in `record_mp4.c`. A process's heap is 2 MB and the writer's
  tables grow with a recording; and it copies every NAL unit twice and gives
  the copies back at once (`MINIMP4_TRANSCODE_SPS_ID`, on as released), so
  the arena is a stack that takes things back.
- Compiled in a file of its own with `-w -Wno-error`, and with
  `record_config.h` first, for the reasons `runtime/upstream/minih264`'s
  README gives.

## What was left out

The test program, the scripts and the test vectors. The demuxer is compiled
- it is in the same header - and unused: `/lib/mp4.lua` is Kosmos's reader.
