# minimp3, vendored

Upstream: <https://github.com/lieff/minimp3>
Licence:  CC0 1.0 Universal (public domain). See `LICENSE`, and the notice
          at the top of `minimp3.h`.

Two files, taken byte for byte: `minimp3.h` and `LICENSE`. **Unmodified**,
which is the rule every vendored thing here follows - what is in the tree is
what the author released, and everything done to it is a build step somebody
can read. No Kosmos copyright line is added; adding one would be modifying
it, which is the one thing that rule forbids.

## What was left out

`minimp3_ex.h`, and everything else in the repository. `minimp3_ex.h` is the
convenience layer: it opens files, seeks, scans for tags, and decodes a whole
stream into one buffer. Every one of those is something Kosmos does
differently - a file comes through the namespace, and a whole song in one
allocation is exactly what the streaming path exists to avoid.

`minimp3.h` alone is the decoder: hand it bytes, get back one frame of PCM
and the number of bytes it used. That is the entire interface the Music
Kit needs.

## Why it needs no Kosmos patches at all

Unusually for something vendored here, nothing had to be worked around.

- **No libm.** The IMDCT and the synthesis filter carry their own tables;
  there is not a `sin`, `cos` or `sqrt` in the file. `runtime/libc/math.c`
  is not involved.
- **No allocation.** `mp3dec_t` is a plain struct of about six kilobytes,
  and the caller owns it. It lives inside the Lua userdata.
- **Sixteen-bit output by default.** `mp3d_sample_t` is `int16_t` unless
  `MINIMP3_FLOAT_OUTPUT` is defined, and signed sixteen-bit is what the
  sound device takes, so nothing converts.
- **NEON is safe here.** On aarch64 it uses `arm_neon.h`, which is the
  compiler's own header and needs no runtime. Kosmos allows that in
  userland: `arch/aarch64/fp.S` saves and restores the whole `q0`-`q31`
  register file on the lazy-FP trap, not just the callee-saved half, so a
  process using vector registers is switched correctly. The kernel is the
  place FP is forbidden, and `-mgeneral-regs-only` makes that a compile
  error there.
