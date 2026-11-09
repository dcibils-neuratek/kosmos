-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- **The LLM Kit's products on the GPU** (`docs/xavier.md`, *X7, the GPU:
-- the design*): kernels in Kosmos's kernel language (`kc.lua`), one copy,
-- which the build compiles into the LLM server's bytes and the host tests
-- run in the emulator.
--
--   local llmgpu = use("/Kosmos/Libraries/llmgpu.lua")
--   local code, info = kc.assemble(llmgpu.q4_0, { simd = 8 })
--
-- **q4_0**: rows of a Q4_0 weight times inputs in Q8_0, a lane a row, and
-- every row's sum made exactly as the processor makes it
-- (`llm_dot_q4_0`): block after block, `sum += float(s) * (sw * xs[b])`,
-- `s` the integer dot of the 32 nibbles less eight with the input's 32
-- values - exact - and `sw` the block's half-precision scale made single
-- from its bits. `kc` fuses nothing and rounds as C does, so the GPU's
-- product is the processor's to the bit.
--
-- What it reads:
--
--   w     the weight, as the file has it: a row is `blocks` blocks of 18
--         bytes - a scale, then 16 bytes of nibbles, the low ones values
--         0-15 and the high ones 16-31 - from byte `w_at`;
--   xq    the inputs' values, 32 signed bytes a block, `blocks * 32` bytes
--         an input - Q8_0 without its scales, so every read is a dword;
--   xs    the inputs' scales as floats, `blocks` an input;
--   out   the sums, as floats: input `t`'s row `r` at `(t * rows + r) * 4`.
--
-- **Two blocks at a time**: a block is 18 bytes, so only every other one
-- begins on a dword, and the data port reads dwords; two are 36 bytes,
-- nine dwords. So `blocks` is even, which every product of phi-2's is; one
-- that is not stays on the processor.

local llmgpu = {}

llmgpu.q4_0 = [[
const int rows, blocks, w_at, inputs;
buffer w;
buffer xq;
buffer xs;
buffer out;

// A half-precision float, made single from its bits as llm_f16_to_f32
// makes it: a normal one by moving its exponent, a subnormal one as its
// mantissa times 2^-24, which is exact either way.
float half(int h) {
  int e = (h >> 10) & 31;
  int mnt = h & 1023;
  int sign = (h >> 15) & 1;
  float v = 0.0;
  if (e == 0) {
    v = float(mnt) * 0.000000059604644775390625;
    if (sign == 1) { v = -v; }
  } else {
    if (e == 31) {
      v = asfloat((sign << 31) | 2139095040 | (mnt << 13));
    } else {
      v = asfloat((sign << 31) | ((e + 112) << 23) | (mnt << 13));
    }
  }
  return v;
}

// Byte k of a dword, signed.
int sbyte(int d, int k) {
  return (d << (24 - 8 * k)) >> 24;
}

// Four bytes of nibbles against the input's values: q's bytes are the
// block's nibble bytes j to j + 3, lo the input's dword of values j to
// j + 3 and hi of values j + 16 to j + 19.
int four(int q, int lo, int hi) {
  int s = 0;
  for (int k = 0; k < 4; k += 1) {
    int b = (q >> (8 * k)) & 255;
    s += ((b & 15) - 8) * sbyte(lo, k) + ((b >> 4) - 8) * sbyte(hi, k);
  }
  return s;
}

// One block's integer dot: its 16 nibble bytes as four dwords, against
// the input's block at byte `x`.
int block(int q0, int q1, int q2, int q3, int x) {
  int s = four(q0, loadi(xq, x), loadi(xq, x + 16));
  s += four(q1, loadi(xq, x + 4), loadi(xq, x + 20));
  s += four(q2, loadi(xq, x + 8), loadi(xq, x + 24));
  s += four(q3, loadi(xq, x + 12), loadi(xq, x + 28));
  return s;
}

void main() {
  int r = group_x * simd + lane;
  if (r < rows) {
    int row = w_at + r * blocks * 18;
    for (int t = 0; t < inputs; t += 1) {
      float sum = 0.0;
      for (int b = 0; b < blocks; b += 2) {
        int at = row + b * 18;
        int d0 = loadi(w, at);
        int d1 = loadi(w, at + 4);
        int d2 = loadi(w, at + 8);
        int d3 = loadi(w, at + 12);
        int d4 = loadi(w, at + 16);
        int x = (t * blocks + b) * 32;
        int xsat = (t * blocks + b) * 4;

        // The first block: its scale the low half of d0, its nibble bytes
        // the high half of d0, d1 to d3, and the low half of d4.
        int q0 = ((d0 >> 16) & 65535) | (d1 << 16);
        int q1 = ((d1 >> 16) & 65535) | (d2 << 16);
        int q2 = ((d2 >> 16) & 65535) | (d3 << 16);
        int q3 = ((d3 >> 16) & 65535) | (d4 << 16);
        int s = block(q0, q1, q2, q3, x);
        sum += float(s) * (half(d0 & 65535) * load(xs, xsat));

        // The second: its scale the high half of d4, its bytes d5 to d8.
        s = block(loadi(w, at + 20), loadi(w, at + 24), loadi(w, at + 28), loadi(w, at + 32), x + 32);
        sum += float(s) * (half((d4 >> 16) & 65535) * load(xs, xsat + 4));
      }
      store(out, (t * rows + r) * 4, sum);
    }
  }
}
]]

return llmgpu
