-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- `user/lib/kc.lua` held to what its programs must do (`docs/cafesa3d-gpu.md`
-- R3): each program compiled, assembled by `eu.lua`, run in the emulator of
-- the execution units (`build/host/eu_run`) in SIMD8 and SIMD16, and every
-- lane's answer held to the same program worked out here - single
-- precision rounded at each step where a float is - and the mistakes it
-- must refuse, each by its line.
--
--   lua tools/test_kc.lua build/host/eu_run

local kc = dofile("user/lib/kc.lua")
local RUN = assert(arg[1], "the emulator's runner, build/host/eu_run")
local OUT = RUN .. ".out"
local checks, fails = 0, {}

local function check(ok, why)
  checks = checks + 1
  if not ok then fails[#fails + 1] = why end
end

local function hex(s) return (s:gsub(".", function(c) return ("%02x"):format(c:byte()) end)) end

local function unhex(h) return (h:gsub("%x%x", function(x) return string.char(tonumber(x, 16)) end)) end

-- A float as single precision rounds it.
local function f32(x) return (string.unpack("<f", string.pack("<f", x))) end

-- Compiled and run: the buffers' bytes, or nil and why.
local function run(src, simd, groups, constants, buffers)
  local code, info = kc.assemble(src, { simd = simd })

  if not code then return nil, info end

  local args = {}

  for _, b in ipairs(buffers) do
    args[#args + 1] = type(b) == "table" and ("%s%d:%s"):format(b.table and "t" or "", #b[1], hex(b[1]))
                      or tostring(b)
  end

  local cmd = ("%s %d %d 1 %s %s %s"):format(RUN, simd, groups, constants ~= "" and hex(constants) or "-",
                                         hex(code), table.concat(args, " "))
  -- Into a file beside the runner, since the host's Lua has no popen.
  os.execute(cmd .. " > " .. OUT .. " 2>&1")
  local f = io.open(OUT)
  local out = f and f:read("a") or "error: the runner said nothing"
  if f then f:close() end

  if out:match("^error") then return nil, out:gsub("%s+$", "") .. "\n" .. info.text end

  local got = {}

  for line in out:gmatch("[^\n]+") do got[#got + 1] = unhex(line) end

  return got, info
end

local function ints(bytes, n)
  local t = {}
  for i = 1, n do t[i] = string.unpack("<i4", bytes, (i - 1) * 4 + 1) end
  return t
end

local function floats(bytes, n)
  local t = {}
  for i = 1, n do t[i] = string.unpack("<f", bytes, (i - 1) * 4 + 1) end
  return t
end

-- Each lane's answer against `want(i)`, the lane's place in the grid.
local function lanes(name, src, want, opts)
  opts = opts or {}

  for _, simd in ipairs { 8, 16 } do
    local groups = opts.groups or 2
    local n = groups * simd
    local got, why = run(src, simd, groups, opts.constants or "", { n * 4, table.unpack(opts.more or {}) })

    if not got then
      check(false, ("%s, SIMD%d: %s"):format(name, simd, tostring(why)))
    else
      local vals = opts.float and floats(got[1], n) or ints(got[1], n)
      local wrong

      for i = 0, n - 1 do
        local w = want(i)
        local ok = opts.float and (w ~= w and vals[i + 1] ~= vals[i + 1]
                                   or math.abs(vals[i + 1] - w) <= (opts.within or 0) * math.max(1, math.abs(w)))
                   or vals[i + 1] == w

        if not ok then wrong = wrong or ("lane %d: %s, not %s"):format(i, tostring(vals[i + 1]), tostring(w)) end
      end

      check(not wrong, ("%s, SIMD%d: %s"):format(name, simd, tostring(wrong)))
    end
  end
end

-- Integers, and where a lane is.
lanes("integers", [[
buffer out;
void main() {
  int x = group_x * simd + lane;
  int a = x * 3 + 1;
  int b = (x << 2) - 7;
  int c = (a & 6) | (b ^ 5);
  store(out, x * 4, c + x % 4 + x / 2 + (10 - x));
}
]], function(x)
  local a, b = x * 3 + 1, x * 4 - 7
  local c = (a & 6) | ((b ~ 5) & 0xffffffff)
  c = c >= 0x80000000 and c - 0x100000000 or c
  return c + x % 4 + x // 2 + (10 - x)
end)

-- Floats, rounded as C rounds them, and conversions.
lanes("floats", [[
const float scale, shift;
buffer out;
void main() {
  int x = group_x * simd + lane;
  float v = float(x) * scale - shift;
  float w = v * v + 0.5 * v - 1.25;
  store(out, x * 4, w / 3.0 + floor(v) - fract(w) + float(int(w * 2.0)));
}
]], function(x)
  local scale, shift = f32(0.37), f32(2.5)
  local v = f32(f32(x * scale) - shift)
  local w = f32(f32(f32(v * v) + f32(0.5 * v)) - 1.25)
  local i = f32(w * 2) >= 0 and math.floor(f32(w * 2)) or math.ceil(f32(w * 2))
  return f32(f32(f32(f32(w / 3) + math.floor(v)) - f32(w - math.floor(w))) + i)
end, { float = true, within = 1e-6, constants = string.pack("<ff", 0.37, 2.5) })

-- Lanes that disagree: Collatz's steps, each lane its own number of them.
lanes("collatz", [[
buffer out;
void main() {
  int n = group_x * simd + lane + 1;
  int steps = 0;
  while (n != 1) {
    if (n % 2 == 0) { n = n / 2; } else { n = 3 * n + 1; }
    steps += 1;
  }
  store(out, (group_x * simd + lane) * 4, steps);
}
]], function(x)
  local n, s = x + 1, 0
  while n ~= 1 do n = n % 2 == 0 and n // 2 or 3 * n + 1; s = s + 1 end
  return s
end, { groups = 4 })

-- break, continue, for, nested ifs, a ternary.
lanes("loops", [[
buffer out;
void main() {
  int x = group_x * simd + lane;
  int sum = 0;
  for (int i = 0; i < 20; i += 1) {
    if (i == x) { continue; }
    if (i > x + 5) {
      if (x % 4 == 0) { break; }
      sum += 100;
    } else {
      sum += i;
    }
  }
  int k = 0;
  while (true) { k += 1; if (k * k > x) { break; } }
  store(out, x * 4, sum * 10 + k + (x > 7 ? 1000 : 2000));
}
]], function(x)
  local sum = 0
  for i = 0, 19 do
    if i ~= x then
      if i > x + 5 then
        if x % 4 == 0 then break end
        sum = sum + 100
      else
        sum = sum + i
      end
    end
  end
  local k = 0
  repeat k = k + 1 until k * k > x
  return sum * 10 + k + (x > 7 and 1000 or 2000)
end, { groups = 3 })

-- Functions inlined, returning early from inside an if.
lanes("functions", [[
buffer out;
int sign(int v) {
  if (v < 0) { return -1; }
  if (v == 0) { return 0; }
  return 1;
}
int clampi(int v, int lo, int hi) { return min(max(v, lo), hi); }
void main() {
  int x = group_x * simd + lane;
  store(out, x * 4, sign(x - 9) * 100 + clampi(x * 3 - 10, 0, 20));
}
]], function(x)
  local s = x - 9 < 0 and -1 or x - 9 == 0 and 0 or 1
  return s * 100 + math.min(math.max(x * 3 - 10, 0), 20)
end, { groups = 3 })

-- inout: a function giving back two things through its caller's variables.
lanes("inout", [[
buffer out;
void split(int v, inout int hi, inout int lo, inout vec3 p) {
  hi = v / 4; lo = v & 3;
  p.y = p.y + float(v);
}
void main() {
  int x = group_x * simd + lane;
  int h = 0; int l = 0; vec3 p = vec3(1.0, 2.0, 3.0);
  split(x * 3, h, l, p);
  store(out, x * 4, h * 100 + l * 10 + int(p.y));
}
]], function(x)
  local v = x * 3
  return (v // 4) * 100 + (v & 3) * 10 + 2 + v
end)

-- vec3: the arithmetic, dot and cross, as the 3D Kit's.
lanes("vectors", [[
buffer out;
void main() {
  int x = group_x * simd + lane;
  vec3 a = vec3(float(x), 2.0, -1.5);
  vec3 b = vec3(0.5, float(x) * 0.25, 3.0);
  vec3 c = cross(a, b) + a * 2.0 - b;
  c.y = c.y * 0.5;
  store(out, x * 4, dot(c, a) + c.x - c.z);
}
]], function(x)
  local ax, ay, az = x, 2.0, -1.5
  local bx, by, bz = 0.5, f32(x * 0.25), 3.0
  local cx = f32(f32(f32(f32(ay * bz) - f32(az * by)) + f32(ax * 2)) - bx)
  local cy = f32(f32(f32(f32(az * bx) - f32(ax * bz)) + f32(ay * 2)) - by)
  local cz = f32(f32(f32(f32(ax * by) - f32(ay * bx)) + f32(az * 2)) - bz)
  cy = f32(cy * 0.5)
  local d = f32(f32(f32(cx * ax) + f32(cy * ay)) + f32(cz * az))
  return f32(f32(d + cx) - cz)
end, { float = true, within = 1e-6 })

-- The GPU's maths: close, not exact.
lanes("maths", [[
buffer out;
void main() {
  int x = group_x * simd + lane;
  float v = float(x) * 0.5 + 0.25;
  vec3 n = normalize(vec3(v, 1.0, 2.0));
  store(out, x * 4, sqrt(v) + rsqrt(v) + sin(v) * cos(v) + pow(v, 1.5) + n.x + length(n));
}
]], function(x)
  local v = x * 0.5 + 0.25
  local l = math.sqrt(v * v + 5)
  return math.sqrt(v) + 1 / math.sqrt(v) + math.sin(v) * math.cos(v) + v ^ 1.5 + v / l + 1
end, { float = true, within = 1e-5 })

-- Buffers in and out: a load3, a sum, a store3, by byte offsets.
do
  local input = {}

  for i = 0, 47 do input[#input + 1] = string.pack("<f", i * 1.5) end

  for _, simd in ipairs { 8, 16 } do
    local got, why = run([[
buffer src, dst;
void main() {
  int x = group_x * simd + lane;
  vec3 v = load3(src, x * 12);
  float s = load(src, x * 4) + v.x;
  store3(dst, x * 12, vec3(v.z, v.y, v.x) * 2.0);
  store(dst, 200 + x * 4, s);
}
]], simd, 16 // simd, "", { { table.concat(input) }, 264 })

    if not got then
      check(false, ("buffers, SIMD%d: %s"):format(simd, tostring(why)))
    else
      local out, bad = got[2], nil

      for x = 0, 15 do
        local z, y, xx = string.unpack("<fff", out, x * 12 + 1)
        local s = string.unpack("<f", out, 200 + x * 4 + 1)

        if z ~= (x * 3 + 2) * 3 or y ~= (x * 3 + 1) * 3 or xx ~= x * 3 * 3 or s ~= x * 1.5 + x * 3 * 1.5 then
          bad = bad or ("lane %d"):format(x)
        end
      end

      check(not bad, ("buffers, SIMD%d: %s wrong"):format(simd, tostring(bad)))
    end
  end
end

-- What it refuses, by line.
local function refused(src, line, words)
  local got, why = kc.compile(src, { simd = 8 })

  check(got == nil and tostring(why):match("^line " .. line .. ":") and tostring(why):find(words, 1, true),
        ("%q was not refused at line %d for %q: %s"):format(src:sub(1, 40), line, words, tostring(why)))
end

refused("buffer out;\nvoid main() {\n  int x = y + 1;\n}\n", 3, "no variable y")
refused("void main() {\n  int x = 1;\n  x = x / 3;\n}\n", 3, "power of two")
refused("int f(int v) {\n  while (v > 0) {\n    return v;\n  }\n  return 0;\n}\nvoid main() { int a = f(2); }\n", 3, "inside a loop")
refused("void main() {\n  break;\n}\n", 2, "outside a loop")
refused("void f(inout int v) { v = 1; }\nvoid main() {\n  f(3);\n}\n", 3, "give it a variable")
refused("void f(inout int v) { v = 1; }\nvoid main() {\n  float a = 0.0;\n  f(a);\n}\n", 4, "not of one type")
refused("void main() {\n  int x = 1\n}\n", 3, "; expected")
refused("const float a;\nvoid main() {\n  a = 2.0;\n}\n", 3, "cannot be assigned")
refused("int f(int v) { return f(v); }\nvoid main() { int a = f(1); }\n", 1, "calls itself")
check(select(2, kc.compile("buffer out;\n", { simd = 8 })) == "line 1: no main", "a program with no main was not refused")

-- The constants, packed as the layout says.
do
  local _, info = kc.compile("const float a; const int b; const vec3 c;\nvoid main() { }\n", { simd = 8 })
  local packed = kc.pack(info, { a = 1.5, b = 7, c = { 1, 2, 3 } })

  check(#packed == 20 and string.unpack("<f", packed, 1) == 1.5 and string.unpack("<i4", packed, 5) == 7
        and string.unpack("<f", packed, 17) == 3, "the constants were not packed as laid out")
end

-- **Tables** (`testing.md` 18.705): a buffer only read, by the sampler's
-- ld_lz - words, floats and vec3s at byte offsets, every lane its own,
-- in SIMD8 and SIMD16; a store to one refused.
do
  local words, floats = {}, {}

  for k = 0, 63 do
    words[#words + 1] = string.pack("<i4", k * 3 + 1)
    floats[#floats + 1] = string.pack("<f", k * 0.5)
  end

  local tab = { table.concat(words), table = true }
  local ftab = { table.concat(floats), table = true }

  for _, simd in ipairs { 8, 16 } do
    local got, why = run([[
table tab, ftab;
buffer out;
void main() {
  int i = group_x * simd + lane;
  vec3 v = load3(ftab, i * 4);
  store(out, i * 4, loadi(tab, i * 4) + loadi(tab, 0));
  store(out, 256 + i * 4, v.x + v.y * 10.0 + v.z * 100.0);
}
]], simd, 32 // simd, "", { tab, ftab, 512 })
    local wrong

    for i = 0, 31 do
      local a = got and string.unpack("<i4", got[3], i * 4 + 1)
      local f = got and string.unpack("<f", got[3], 256 + i * 4 + 1)
      local want_f = i * 0.5 + (i + 1) * 0.5 * 10 + (i + 2) * 0.5 * 100

      if not got or a ~= i * 3 + 1 + 1 or math.abs(f - want_f) > 1e-3 then
        wrong = wrong or ("lane %d: %s, %s"):format(i, tostring(a), tostring(f))
      end
    end

    check(got ~= nil and wrong == nil, ("tables, SIMD%d: %s"):format(simd, tostring(wrong or why)))
  end

  local _, why = kc.compile("table tab;\nvoid main() {\n  store(tab, 0, 1);\n}\n", { simd = 8 })

  check(tostring(why):match("line 3: tab is a table, which is only read") ~= nil,
        "a store to a table was not refused: " .. tostring(why))

  local _, info = kc.compile("buffer a;\ntable b;\nbuffer c;\nvoid main() { }\n", { simd = 8 })

  check(info and info.views[1] == "bytes" and info.views[2] == "table" and info.views[3] == "bytes",
        "a kernel's tables and buffers were not said, in order")
end

-- **What the code is, not only what it computes** (`testing.md` 18.701):
-- the copies and second compares that were a third of the tracer's
-- instructions must not come back unnoticed. Counted in what kc writes.
do
  local function count(src, pattern)
    local text = assert(kc.compile(src, { simd = 8 }))
    local n = 0

    for line in text:gmatch("[^\n]+") do
      if line:gsub("^%([^)]*%)%s*", ""):match(pattern) then n = n + 1 end
    end

    return n
  end

  local head = "const int n;\nbuffer out;\n"

  -- A sum into its variable: the add writes it, no mov. Four register
  -- movs are the lane's widening, a and b set from it and the store's
  -- payload; a fifth is the copy.
  check(count(head .. "void main() {\n  int a = lane;\n  int b = lane;\n  a = a + b * 3;\n"
              .. "  store(out, lane * 4, a);\n}\n", "^mov %([^)]*%) r%d+%.0<1>:d r%d+%.0<8") <= 4,
        "an assignment is written through a mov again")

  -- A comparison steering an if: one cmp, into the flag.
  check(count(head .. "void main() {\n  int v = 0;\n  if (lane < n) { v = 1; }\n"
              .. "  store(out, lane * 4, v);\n}\n", "^cmp") == 1,
        "a comparison in an if is two cmps again")

  -- An accessor of one return with a constant: a multiply, an add and the
  -- load - no constant in a register, no result copied out. Six movs are
  -- every kernel's five (the lanes, the group, the thread's end) and the
  -- store's payload; the old way was nine.
  check(count(head .. "float get(int o, int k) { return load(out, o * 192 + k * 4); }\n"
              .. "void main() {\n  float f = get(lane, 25);\n  store(out, lane * 4, f);\n}\n",
              "^mov") <= 6,
        "a call of one return with a constant is copied about again")
end

if #fails > 0 then
  print(("FAIL: %d of %d kc checks"):format(#fails, checks))
  for _, f in ipairs(fails) do print("  " .. f) end
  os.exit(1)
end

print(("PASS: %d kc checks (integers, floats, lanes that disagree, loops, functions, vec3, the GPU's "
       .. "maths and buffers, in SIMD8 and SIMD16 through the emulator; what is refused, by line; and "
       .. "no copies or second compares where none are needed)"):format(checks))
