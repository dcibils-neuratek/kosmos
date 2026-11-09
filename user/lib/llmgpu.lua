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
--   w     the weight, **laid out for the GPU** from byte `w_at`: the rows in
--         tiles of 16, and a tile's rows side by side - for each pair of
--         blocks, its nine dwords, and each dword the sixteen rows' in turn
--         (`llmgpu.tile` says where). A block is as the file has it: 18
--         bytes, a scale, then 16 bytes of nibbles, the low ones values 0-15
--         and the high ones 16-31;
--   xq    the inputs' values, 32 signed bytes a block, `blocks * 32` bytes
--         an input - Q8_0 without its scales, so every read is a dword;
--   xm    for each pair of an input's blocks, 16 bytes: the two scales as
--         floats, then each block's 32 values summed, as ints - so the
--         nibbles' "less eight" is taken once a block, as `8 * sum`, rather
--         than once a value: the integer dot is the same integer;
--   out   the sums, as floats: input `t`'s row `r` at `(t * rows + r) * 4`.
--
-- **Two blocks at a time**: a block is 18 bytes, so only every other one
-- begins on a dword, and the data port reads dwords; two are 36 bytes,
-- nine dwords. So `blocks` is even, which every product of phi-2's is; one
-- that is not stays on the processor.
--
-- **Why the tiles** (`testing.md` 18.729): with the rows as the file has
-- them, the sixteen lanes of a read are sixteen rows apart - sixteen
-- cache lines for sixteen dwords - and on the M700 a product ran at about
-- 1.5 GB/s. Side by side, a read is one line of 64 bytes. The arithmetic
-- is untouched, so the sums are still the processor's to the bit.
--
-- **And the inputs read once a pair** (18.729): every lane wants the same
-- input values, and read lane by lane they were sixteen messages a pair
-- of blocks, and two each for the scales and the sums - two thirds of
-- what the kernel asked the memory for, since the GPU's buffers are not
-- in its L3. Now a lane reads one dword of the pair's 64 bytes, and
-- `lanev` gives each to every lane: three messages for the twenty. So
-- every lane runs the loop - a tile's missing rows are nought and there
-- to be read - and only the store asks whether the row is one.

local llmgpu = {}

llmgpu.q4_0 = [[
const int rows, blocks, w_at, inputs;
buffer w;
buffer xq;
buffer xm;
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

// Four bytes of nibbles against the input's values, the nibbles as they
// are (0-15): q's bytes are the block's nibble bytes j to j + 3, lo the
// input's dword of values j to j + 3 and hi of values j + 16 to j + 19.
// Written out, every shift a constant: as a loop, with the shifts worked
// out at run time, it was twice the instructions (18.729).
int four(int q, int lo, int hi) {
  return (q & 15) * ((lo << 24) >> 24)
       + ((q >> 4) & 15) * ((hi << 24) >> 24)
       + ((q >> 8) & 15) * ((lo << 16) >> 24)
       + ((q >> 12) & 15) * ((hi << 16) >> 24)
       + ((q >> 16) & 15) * ((lo << 8) >> 24)
       + ((q >> 20) & 15) * ((hi << 8) >> 24)
       + ((q >> 24) & 15) * (lo >> 24)
       + ((q >> 28) & 15) * (hi >> 24);
}

// One block's integer dot: its 16 nibble bytes as four dwords, against
// the input's block, whose eight dwords are lanes 0 to 7 of `xv`, less
// eight times the block's values' sum.
int block(int q0, int q1, int q2, int q3, int xv, int xsum) {
  int s = four(q0, lanev(xv, 0), lanev(xv, 4));
  s += four(q1, lanev(xv, 1), lanev(xv, 5));
  s += four(q2, lanev(xv, 2), lanev(xv, 6));
  s += four(q3, lanev(xv, 3), lanev(xv, 7));
  return s - 8 * xsum;
}

// Input u's sum, a pair of blocks further on: the pair's two blocks of
// nibbles, q and e, their scales h0 and h1, against input u's blocks b and
// b + 1 - the first block's term added, then the second's, as the
// processor adds them.
float pair(float sum, int q0, int q1, int q2, int q3, int e0, int e1, int e2, int e3,
           float h0, float h1, int u, int b) {
  int x = (u * blocks + b) * 32;
  int xa = loadi(xq, x + (lane & 7) * 4);
  int xb = loadi(xq, x + 32 + (lane & 7) * 4);
  int m = loadi(xm, (u * blocks + b) * 8 + (lane & 3) * 4);
  float s = sum + float(block(q0, q1, q2, q3, xa, lanev(m, 2))) * (h0 * asfloat(lanev(m, 0)));
  return s + float(block(e0, e1, e2, e3, xb, lanev(m, 3))) * (h1 * asfloat(lanev(m, 1)));
}

// **Four inputs a pass** (18.729): a pair of blocks read once and met by
// four inputs, where one at a time read the weight again for each. A
// pass's inputs past the last are the last again, worked out and not
// stored. Each input's sum is its own, in its own order, so the answers
// are the same.
void main() {
  int r = group_x * simd + lane;
  int row = w_at + (r >> 4) * (blocks >> 1) * 576 + (r & 15) * 4;
  for (int t = 0; t < inputs; t += 4) {
    int t1 = min(t + 1, inputs - 1);
    int t2 = min(t + 2, inputs - 1);
    int t3 = min(t + 3, inputs - 1);
    float s0 = 0.0;
    float s1 = 0.0;
    float s2 = 0.0;
    float s3 = 0.0;
    for (int b = 0; b < blocks; b += 2) {
      int at = row + (b >> 1) * 576;
      int d0 = loadi(w, at);
      int d1 = loadi(w, at + 64);
      int d2 = loadi(w, at + 128);
      int d3 = loadi(w, at + 192);
      int d4 = loadi(w, at + 256);

      // The first block: its scale the low half of d0, its nibble bytes
      // the high half of d0, d1 to d3, and the low half of d4. The
      // second: its scale the high half of d4, its bytes the next four.
      int q0 = ((d0 >> 16) & 65535) | (d1 << 16);
      int q1 = ((d1 >> 16) & 65535) | (d2 << 16);
      int q2 = ((d2 >> 16) & 65535) | (d3 << 16);
      int q3 = ((d3 >> 16) & 65535) | (d4 << 16);
      int e0 = loadi(w, at + 320);
      int e1 = loadi(w, at + 384);
      int e2 = loadi(w, at + 448);
      int e3 = loadi(w, at + 512);
      float h0 = half(d0 & 65535);
      float h1 = half((d4 >> 16) & 65535);

      s0 = pair(s0, q0, q1, q2, q3, e0, e1, e2, e3, h0, h1, t, b);
      if (inputs > 1) {
        s1 = pair(s1, q0, q1, q2, q3, e0, e1, e2, e3, h0, h1, t1, b);
        s2 = pair(s2, q0, q1, q2, q3, e0, e1, e2, e3, h0, h1, t2, b);
        s3 = pair(s3, q0, q1, q2, q3, e0, e1, e2, e3, h0, h1, t3, b);
      }
    }
    if (r < rows) {
      store(out, (t * rows + r) * 4, s0);
      if (t + 1 < inputs) { store(out, (t1 * rows + r) * 4, s1); }
      if (t + 2 < inputs) { store(out, (t2 * rows + r) * 4, s2); }
      if (t + 3 < inputs) { store(out, (t3 * rows + r) * 4, s3); }
    }
  }
}
]]

-- Where the tiled weight keeps dword `j` (0-8) of row `r`'s pair of
-- blocks `p`, in bytes, for a weight of `blocks` blocks a row - the one
-- statement of the layout, which `llm_gpu.c` and the tests follow.
llmgpu.TILE = 16

function llmgpu.tile(r, p, j, blocks)
  return ((r // 16) * (blocks // 2) + p) * 576 + j * 64 + (r % 16) * 4
end

return llmgpu
