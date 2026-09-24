# minih264, vendored

Upstream: <https://github.com/lieff/minih264>, commit
`b0baea7a80ef9d12da97301dd1099b8791b5ba43` (10 December 2020), downloaded on
24 September 2026 as the tarball GitHub serves for that commit.
Licence:  CC0 1.0 Universal (public domain). See `LICENSE`, and the notice at
          the top of `minih264e.h`.

Two files, taken byte for byte: `minih264e.h` and `LICENSE`. **Unmodified**,
as every vendored thing here is.

    661afe7802b4e1174e8389632f3ee437b903e036a9c91a7f035830947c6a7aae  minih264e.h
    6a1ee543e5282cd9061881edf462e6fdab181f328da71fc2c9a6950a80e94d01  LICENSE

## What it is for

The Record Kit's encoder (`user/kits/record`, `roadmap.md` 6d 8f): the
Camera app records to H.264, Diego's choice - "as all modern video players
are h264". Baseline profile, from planar 4:2:0, into memory the caller gives
it: nothing is allocated.

## How it is built, as build steps rather than edits

`user/kits/record/record_config.h` holds all of them, included before the
header by every file that includes it:

- **No threads and no SVC** (`H264E_MAX_THREADS 0`, `H264E_SVC_API 0`).
- **Little-endian, said outright.** Its endianness test knows Linux and Apple
  and stops anywhere else, and its Apple branch reads `BYTE_ORDER`, which
  strict C11 does not define - so on the Mac both sides of the comparison
  were undefined and it took itself for big-endian. The ARMCC branch says
  little-endian, and `__ARMCC_VERSION` is defined to take it.
- **`vtbl2q_u8` is `vqtbl2_u8`** on Apple's own compiler for arm64, the only
  place that branch is reached - the Mac's copy of the host test.
- Compiled in a file of its own, `record_h264.c`, with `-w -Wno-error`: its
  warnings are not ours to fix, and `minimp4` carries a copy of its
  bitstream writer, so the two in one file are the same names twice.

## What was left out

The rest of the repository: the ARM32 assembly in `asm/`, the test program,
the scripts and the 30 MB of test vectors.
