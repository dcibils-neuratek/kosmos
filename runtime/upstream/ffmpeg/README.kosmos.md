# FFmpeg, vendored: the H.264 decoder

Upstream: <https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz>, downloaded on
19 September 2026, sha256
`8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e`, and
checked against FFmpeg's release signature on 24 September: "Good signature
from FFmpeg release signing key", key
`FCF986EA15E6E293A5644F10B4322F04D67658D8`.
Licence: LGPL 2.1 or later, as `configure` reported for this configuration.
See `COPYING.LGPLv2.1` and `LICENSE.md`, and the notice at the top of every
file.

**Unmodified**, as every vendored thing here is. What is here is not all of
FFmpeg: it is the files one decoder needs, and every one of them is byte
for byte as released.

## How the files were chosen: by the linker

`tools/ffmpeg_vendor.py` does all of it, and running it again is how a new
FFmpeg arrives, or a second decoder:

1. The tarball is checked against the sum above and unpacked in
   `build/ffmpeg/`.
2. `configure` is run for a freestanding AArch64 target with everything
   disabled but `--enable-decoder=h264`: no demuxers (`/lib/mp4.lua` is the
   demuxer), no parsers (a sample from an MP4 is a whole access unit), no
   threads, no assembly.
3. Every object FFmpeg's own build would compile for `libavcodec` and
   `libavutil` is compiled - 173 of them - and the kit's entry points are
   linked against them with the map switched on. The archive members the
   linker pulled in are the closure: **95 objects**. Their sources and
   every header they include are copied here.
4. It writes `user/kits/ffmpeg/ffmpeg.mk`, the object list the Makefile
   compiles and FFmpeg's own preprocessor flags, so the list cannot drift
   from what is here.

`tests/ref/fate/` holds FFmpeg's checksums for the eighteen conformance
streams `tools/test_h264.c` decodes (`testing.md` 18.181) - FFmpeg's word on
its own decoder, which is what the Mac holds the port to.

## How it is built, as build steps rather than edits

- **The configuration is Kosmos's**, in `user/kits/ffmpeg/config/`, and it
  is `configure`'s output corrected by rules the script applies and prints.
  `configure` tests a function by linking a program that calls it, and
  there is nothing for it to link against here, so it concluded that
  nothing exists. The rules: a maths or system function is present exactly
  when `runtime/include/` declares it - `libavutil/libm.h` defines a static
  fallback for any it believes absent, and a static definition after this
  system's declaration is an error, so this has to be true in both
  directions; no system header counts, `unistd.h` included, which is there
  and empty on purpose; and the three things `--disable-asm` forgets with
  the assembly - both Kosmos targets are 64-bit, little-endian, load from
  any address and count leading zeros in one instruction.
- **`-DHAVE_AV_CONFIG_H`**, which FFmpeg's `library.mak` adds to every
  library object, and without which its headers never include `config.h`.
- **FFmpeg's include path first**, ahead of the userland's, and its own
  `-std=c17 -O3` and `CPPFLAGS`, including the `compat/` directory that
  stands in for `<stdatomic.h>` in a build without threads.
- **`-w -Wno-error`**, because FFmpeg's warnings are not ours to fix.
  `user/kits/ffmpeg/h264_core.c`, the one Kosmos file that includes its
  headers, keeps every warning.

## What it asked of Kosmos

Standard C, and nothing on the wrong side of `design.md` §17: the rest of
`<time.h>` (`runtime/libc/time.c`, `testing.md` 18.183), `strtoll`,
`strtoull`, `logf`, the errno names its error table knows, and nine more of
musl's maths functions for its expression evaluator. And libgcc on the
userland's link, for `av_sscanf`'s `long double`, which on AArch64 is
arithmetic in calls.

`libavutil/file.c` and `file_open.c` did not compile - they are `open`,
`read` and `fstat` - and nothing in the closure calls them.

## What is linked and never run

The decoder brings FFmpeg's option system with it, because every codec
context is an `AVOptions` object, and the option system brings a colour
parser, a time parser and an expression evaluator. The kit sets no option
by string, so none of it runs. One corner is worth knowing: the colour
parser's `"random"` calls `av_get_random_seed`, which waits for `clock()` to
move, and here `clock()` answers "not available" - so that call would not
return. Nothing reaches it.

## What it costs

1.4 MB of code and 220 KB of tables, shared by every process like the rest
of the image's read-only half; and 812 KB of `.bss`, which is not shared
yet - every process carries a copy (`roadmap.md` 6j).
