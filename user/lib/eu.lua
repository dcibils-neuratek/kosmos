-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The GPU's own instructions, assembled on Kosmos (`docs/compute.md` C1,
-- decision D1): a kernel's text in, the execution units' 128-bit
-- instructions out, for Intel's generation 9 - Skylake, the M700's HD 530.
--
--   local eu = use("/Kosmos/Libraries/eu.lua")
--   local code, why = eu.assemble(text)      -- bytes, or nil and "line 7: ..."
--
-- **The syntax is the one Mesa's tools print** (`gentool disasm -v`), so a
-- kernel can be read beside what Mesa says of it:
--
--   mov (16|M0)  r10.0<1>:f   r11.0<8;8,1>:f
--   add (16|M0)  r10.0<1>:d   r11.0<8;8,1>:d   1:d
--   cmp (16|M0)  (lt)f0.0  null<1>:f  r24.0<8;8,1>:f  0x40800000:f
--   (f0.0) sel (16|M0)  r30.0<1>:f  r31.0<8;8,1>:f  r32.0<8;8,1>:f
--   mad (16)  r12<0>:f  r1<0;0>.r:f  r10<0;0>:f  r1.2<0>.r:f {Align16}
--   (f0.0.any16h) jmpi (1)  loop
--   send.ts (16|M0)  null  r112  0x00000000  0x02000010  {EOT}
--   loop:
--
-- A label is a name and a colon on a line of its own; `//` begins a
-- comment. A branch to a label is an offset in bytes from the branch
-- itself, as Mesa's assembler takes it.
--
-- **The subset a compute kernel needs**, and every form of it held bit for
-- bit to Mesa's assembler by `tools/test_eu.lua` over `tools/eu_corpus.txt`:
-- moves and conversions; add, mul, three-source mad; and, or, xor, shl,
-- shr; cmp into a flag, predication, sel; jmpi; send. Each field's bits are
-- where Mesa's `pre_xe.json` puts them. A line outside the subset is
-- refused with its number and why, never guessed at.
--
-- This turns text into numbers once, when a program starts: Lua, in the
-- program's own process, whose bug is a stack trace. Nothing per pixel.

local eu = {}

--------------------------------------------------------------------------
-- An instruction is four 32-bit words, and a field a run of its bits.
--------------------------------------------------------------------------

local function field(w, lo, hi, v)
  local width = hi - lo + 1
  local mask = (1 << width) - 1

  assert(v >= 0 and v <= mask, "a field's value out of its width")

  for bit = lo, hi do
    local word, at = bit // 32 + 1, bit % 32
    local b = (v >> (bit - lo)) & 1

    w[word] = (w[word] & ~(1 << at)) | (b << at)
  end
end

local function bytes(w)
  return string.pack("<I4I4I4I4", w[1] & 0xffffffff, w[2] & 0xffffffff,
                     w[3] & 0xffffffff, w[4] & 0xffffffff)
end

--------------------------------------------------------------------------
-- The tables: opcodes, types, files, regions, conditions, predicates and
-- the sends' units, as generation 9 numbers them.
--------------------------------------------------------------------------

eu.OPCODES = {
  mov = 0x01, sel = 0x02, ["and"] = 0x05, ["or"] = 0x06, xor = 0x07,
  shr = 0x08, shl = 0x09, cmp = 0x10, jmpi = 0x20, send = 0x31,
  add = 0x40, mul = 0x41, mad = 0x5b,
}

-- A register operand's type; an immediate's are the same but for :v.
local TYPES = { ud = 0, d = 1, uw = 2, w = 3, ub = 4, b = 5, df = 6, f = 7 }
local IMM_TYPES = { ud = 0, d = 1, uw = 2, w = 3, v = 6, f = 7 }
local THREE_TYPES = { f = 0, d = 1, ud = 2, df = 3 }

local FILE_ARF, FILE_GRF, FILE_IMM = 0, 1, 3

local EXEC = { [1] = 0, [2] = 1, [4] = 2, [8] = 3, [16] = 4, [32] = 5 }
local VSTRIDE = { [0] = 0, [1] = 1, [2] = 2, [4] = 3, [8] = 4, [16] = 5, [32] = 6 }
local WIDTH = { [1] = 0, [2] = 1, [4] = 2, [8] = 3, [16] = 4 }
local HSTRIDE = { [0] = 0, [1] = 1, [2] = 2, [4] = 3 }

local CONDITIONS = { eq = 1, ne = 2, gt = 3, ge = 4, lt = 5, le = 6 }

-- Align1's predicates: the flag lane by lane, or any or all of a group.
local PREDICATES = { [""] = 1, any2h = 2, all2h = 3, any4h = 4, all4h = 5, any8h = 6,
                     all8h = 7, any16h = 10, all16h = 11, any32h = 12, all32h = 13 }

-- The units a send can address, by the name Mesa prints.
local SFIDS = { ts = 7, hdc1 = 12, hdc0 = 10, hdc_ro = 9, sampler = 2 }

-- The architecture registers this subset names.
local ARF = { null = 0x00, acc0 = 0x20, ip = 0xa0 }

local TYPE_BYTES = { ud = 4, d = 4, f = 4, uw = 2, w = 2, ub = 1, b = 1, df = 8 }

--------------------------------------------------------------------------
-- Operands.
--------------------------------------------------------------------------

local function fail(why) error({ why = why }, 0) end

-- `r10.2<8;8,1>:f`, `-r12.0<8;8,1>:f`, `(abs)r3<0;1,0>:d`, `null<1>:f`,
-- `acc0`, an immediate `1:d` / `0x3f800000:f` / `0x76543210:v`.
local function operand(text, is_dst)
  local o = {}

  if text:sub(1, 5) == "(abs)" then o.abs = true; text = text:sub(6) end
  if text:sub(1, 1) == "-" and not text:match("^%-%d") then o.neg = true; text = text:sub(2) end

  -- An immediate: a number and its type - or a bare hexadecimal number,
  -- a send's descriptor, taken as :ud.
  if text:match("^0x%x+$") then
    return { file = FILE_IMM, imm = tonumber(text:sub(3), 16) & 0xffffffff, type = "ud" }
  end

  local num, ty = text:match("^(%-?0?x?[%x]+):(%a+)$")

  if num and not text:match("^r%d") and not text:match("^null") and not text:match("^acc") then
    local v = num:match("^0x") and tonumber(num:sub(3), 16) or tonumber(num)

    if not v or not IMM_TYPES[ty] then fail("an immediate I cannot read: " .. text) end

    o.file, o.imm, o.type = FILE_IMM, v & 0xffffffff, ty
    return o
  end

  local name, rest = text:match("^(%a+%d*)(.*)$")

  if not name then fail("an operand I cannot read: " .. text) end

  if name:match("^r%d+$") then
    o.file, o.nr = FILE_GRF, tonumber(name:sub(2))
    if o.nr > 127 then fail("no register " .. name) end
  elseif ARF[name] then
    o.file, o.nr = FILE_ARF, ARF[name]
  else
    fail("no register " .. name)
  end

  local sub = rest:match("^%.(%d+)")

  o.sub = sub and tonumber(sub) or 0
  rest = rest:gsub("^%.%d+", "")

  local region = rest:match("^<([%d;,]+)>")

  if region then
    local v, w, h = region:match("^(%d+);(%d+),(%d+)$")

    if v then
      o.vstride, o.width, o.hstride = tonumber(v), tonumber(w), tonumber(h)
    else
      local v2, w2 = region:match("^(%d+);(%d+)$")

      if v2 then
        o.vstride, o.width = tonumber(v2), tonumber(w2)      -- align16's <v;w>
      else
        o.hstride = tonumber(region)                           -- a destination's <h>, or <v>
        o.short = true
      end
    end

    rest = rest:gsub("^<[%d;,]+>", "")
  end

  if rest:match("^%.r") then o.rep = true; rest = rest:gsub("^%.r", "") end

  o.type = rest:match("^:(%a+)$")

  if rest ~= "" and not o.type then fail("an operand I cannot read: " .. text) end

  return o
end

-- A register's subregister in bytes, from its number in its type's units.
local function sub_bytes(o)
  return o.sub * (TYPE_BYTES[o.type or "ud"] or 4)
end

--------------------------------------------------------------------------
-- The fields every instruction shares.
--------------------------------------------------------------------------

local function controls(w, ins)
  field(w, 0, 6, ins.opcode)
  field(w, 21, 23, EXEC[ins.exec] or fail("no execution size " .. tostring(ins.exec)))

  if ins.align16 then field(w, 8, 8, 1) end

  if ins.pred then
    field(w, 16, 19, ins.pred.control)
    if ins.pred.inv then field(w, 20, 20, 1) end
    field(w, 33, 33, ins.pred.nr)
    field(w, 32, 32, ins.pred.sub)
  end

  if ins.cond then
    field(w, 24, 27, ins.cond.code)
    field(w, 33, 33, ins.cond.nr)
    field(w, 32, 32, ins.cond.sub)
  end

  if ins.nomask then field(w, 34, 34, 1) end
  if ins.eot then field(w, 127, 127, 1) end
end

-- An align1 source's 25-bit operand, at `base`.
local function src_operand(w, base, o)
  field(w, base, base + 4, sub_bytes(o))
  field(w, base + 5, base + 12, o.nr)
  if o.abs then field(w, base + 13, base + 13, 1) end
  if o.neg then field(w, base + 14, base + 14, 1) end

  local v, wd, h = o.vstride, o.width, o.hstride

  if o.short then v, wd, h = o.hstride, 1, 0 end   -- `<v>` is `<v;1,0>`

  field(w, base + 16, base + 17, HSTRIDE[h or 1] or fail("no horizontal stride " .. tostring(h)))
  field(w, base + 18, base + 20, WIDTH[wd or 8] or fail("no width " .. tostring(wd)))
  field(w, base + 21, base + 24, VSTRIDE[v or 8] or fail("no vertical stride " .. tostring(v)))
end

local function dst_operand(w, o)
  field(w, 35, 36, o.file)
  field(w, 37, 40, TYPES[o.type or "ud"] or fail("no type " .. tostring(o.type)))
  field(w, 48, 52, sub_bytes(o))
  field(w, 53, 60, o.nr)
  field(w, 61, 62, HSTRIDE[o.hstride or 1] or fail("no stride " .. tostring(o.hstride)))
end

local function encode_align1(ins)
  local w = { 0, 0, 0, 0 }
  local d, s0, s1 = ins.ops[1], ins.ops[2], ins.ops[3]

  controls(w, ins)
  dst_operand(w, d)

  field(w, 41, 42, s0.file)

  if s0.file == FILE_IMM then
    -- The type twice: the immediate's own field and the second source's,
    -- as Mesa's assembler writes a one-source instruction's.
    field(w, 43, 46, IMM_TYPES[s0.type])
    field(w, 91, 94, IMM_TYPES[s0.type])
    field(w, 96, 127, s0.imm)
    return bytes(w)
  end

  field(w, 43, 46, TYPES[s0.type or "ud"] or fail("no type " .. tostring(s0.type)))
  src_operand(w, 64, s0)

  if s1 then
    field(w, 89, 90, s1.file)

    if s1.file == FILE_IMM then
      field(w, 91, 94, IMM_TYPES[s1.type])
      field(w, 96, 127, s1.imm)
    else
      field(w, 91, 94, TYPES[s1.type or "ud"] or fail("no type " .. tostring(s1.type)))
      src_operand(w, 96, s1)
    end
  end

  return bytes(w)
end

-- Three sources, align16: `mad`. Each source 21 bits - replicate, a
-- swizzle, a subregister in dwords, the register - the destination its
-- write mask, subregister and register.
local function encode_three(ins)
  local w = { 0, 0, 0, 0 }
  local d = ins.ops[1]
  local srcs = { ins.ops[2], ins.ops[3], ins.ops[4] }
  local ty = THREE_TYPES[d.type or "f"] or fail("no three-source type " .. tostring(d.type))

  ins.align16 = true
  controls(w, ins)
  field(w, 43, 45, ty)
  field(w, 46, 48, ty)
  field(w, 49, 52, 0xf)                                 -- xyzw
  field(w, 53, 55, (sub_bytes(d) // 4) & 7)
  field(w, 56, 63, d.nr)

  for i, s in ipairs(srcs) do
    local base = 64 + (i - 1) * 21
    local sub = sub_bytes(s) // 4

    if s.file ~= FILE_GRF then fail("a three-source operand must be a register") end

    if s.rep then field(w, base, base, 1) end
    field(w, base + 1, base + 8, s.rep and 0 or 0xe4)   -- .r, or xyzw
    field(w, base + 9, base + 11, sub & 7)
    field(w, base + 12, base + 19, s.nr)
    field(w, base + 20, base + 20, (sub >> 3) & 1)

    if s.abs then field(w, 37 + (i - 1) * 2, 37 + (i - 1) * 2, 1) end
    if s.neg then field(w, 38 + (i - 1) * 2, 38 + (i - 1) * 2, 1) end
  end

  return bytes(w)
end

-- `send.<unit> (n|M0) dst src0 exdesc desc {EOT}`: the unit in bits 24-27,
-- the descriptor in the last dword with end-of-thread at its top.
local function encode_send(ins)
  local w = { 0, 0, 0, 0 }
  local d, s0, ex, desc = ins.ops[1], ins.ops[2], ins.ops[3], ins.ops[4]

  if not (d and s0 and ex and desc) then fail("a send is: destination, payload, ex-descriptor, descriptor") end
  if ex.file ~= FILE_IMM or ex.imm ~= 0 then fail("this subset's sends take an ex-descriptor of 0") end
  if desc.file ~= FILE_IMM then fail("a send's descriptor is a number") end

  controls(w, ins)
  field(w, 24, 27, ins.sfid)
  field(w, 35, 36, d.file)
  field(w, 37, 40, TYPES[d.type or "ud"])
  field(w, 53, 60, d.nr)
  field(w, 41, 42, s0.file)
  field(w, 69, 76, s0.nr)
  field(w, 89, 90, FILE_IMM)
  field(w, 96, 126, desc.imm & 0x7fffffff)

  return bytes(w)
end

-- `jmpi (1) offset:d`: the instruction pointer, moved by `offset` bytes
-- from the branch itself.
local function encode_jmpi(ins, offset)
  local w = { 0, 0, 0, 0 }

  controls(w, ins)
  field(w, 35, 36, FILE_ARF)
  field(w, 53, 60, ARF.ip)
  field(w, 41, 42, FILE_ARF)
  field(w, 69, 76, ARF.ip)
  field(w, 85, 88, VSTRIDE[4])
  field(w, 89, 90, FILE_IMM)
  field(w, 91, 94, IMM_TYPES.d)
  field(w, 96, 127, offset & 0xffffffff)

  return bytes(w)
end

--------------------------------------------------------------------------
-- A line.
--------------------------------------------------------------------------

local function words(text)
  local out = {}

  for t in text:gmatch("%S+") do out[#out + 1] = t end

  return out
end

-- One instruction's text, parsed: its opcode, controls and operands; a
-- branch's target left as a label or a number for the second pass.
local function parse(text)
  local ins = { ops = {} }
  local t = words(text)
  local i = 1

  -- A predicate and NoMask: (f0.0), (~f0.0), (f0.0.any16h), (W), (W&f0.0).
  if t[i] and t[i]:match("^%(") then
    local p = t[i]:match("^%((.-)%)$") or fail("a predicate I cannot read: " .. t[i])

    if p:sub(1, 1) == "W" then
      ins.nomask = true
      p = p:gsub("^W&?", "")
    end

    if p ~= "" then
      local inv, nr, sub, kind = p:match("^(~?)f(%d)%.(%d)%.?(%a*%d*%a*)$")

      if not nr or not PREDICATES[kind] then fail("a predicate I cannot read: (" .. p .. ")") end

      ins.pred = { inv = inv == "~", nr = tonumber(nr), sub = tonumber(sub), control = PREDICATES[kind] }
    end

    i = i + 1
  end

  local op = t[i] or fail("no instruction")
  local name, unit = op:match("^(%a+)%.?(%w*)$")

  if not name or not eu.OPCODES[name] then fail("no instruction " .. op .. " in this assembler's subset") end

  ins.name, ins.opcode = name, eu.OPCODES[name]

  if name == "send" then
    ins.sfid = SFIDS[unit] or fail("no unit " .. unit .. " a send here can address")
  elseif unit ~= "" then
    fail("no instruction " .. op)
  end

  i = i + 1

  -- The execution size, and its channel group: (16) or (16|M0).
  local exec = t[i] and t[i]:match("^%((%d+)|?M?0?%)$")

  if not exec then fail("no execution size after " .. op) end

  ins.exec = tonumber(exec)
  i = i + 1

  -- A condition writing a flag: (lt)f0.0.
  if t[i] and t[i]:match("^%(%a+%)f%d%.%d$") then
    local c, nr, sub = t[i]:match("^%((%a+)%)f(%d)%.(%d)$")

    ins.cond = { code = CONDITIONS[c] or fail("no condition " .. c), nr = tonumber(nr), sub = tonumber(sub) }
    i = i + 1
  end

  -- The operands, then the braces.
  while t[i] do
    local tok = t[i]

    if tok:sub(1, 1) == "{" then
      local flags = text:match("{(.-)}") or ""

      for f in flags:gmatch("[^,%s]+") do
        if f == "EOT" then ins.eot = true
        elseif f == "Align16" then ins.align16 = true
        else fail("no option " .. f .. " here") end
      end

      break
    end

    if name == "jmpi" and #ins.ops == 0 then
      local n = tok:match("^(%-?0?x?%x+):d$")

      if n then
        ins.target = n:match("^0x") and tonumber(n:sub(3), 16) or tonumber(n)
        if ins.target >= 0x80000000 then ins.target = ins.target - 0x100000000 end
      elseif tok:match("^[%a_][%w_]*$") then
        ins.label = tok
      else
        fail("a branch goes to a label or an offset: " .. tok)
      end

      ins.ops[1] = true
    else
      ins.ops[#ins.ops + 1] = operand(tok)
    end

    i = i + 1
  end

  return ins
end

local function encode(ins, offset)
  if ins.name == "jmpi" then return encode_jmpi(ins, offset or 0) end
  if ins.name == "send" then return encode_send(ins) end
  if ins.name == "mad" then return encode_three(ins) end

  if #ins.ops < 2 then fail(ins.name .. " needs a destination and a source") end
  if ins.align16 then fail("only mad is three-source and align16 here") end

  return encode_align1(ins)
end

-- One line, alone: its sixteen bytes, or nil and why. A branch must give
-- its offset as a number here; a label needs the whole kernel.
function eu.encode_line(text)
  local ok, out = pcall(function()
    local ins = parse(text)

    if ins.label then fail("a label needs the whole kernel") end

    return encode(ins, ins.target)
  end)

  if ok then return out end

  return nil, type(out) == "table" and out.why or tostring(out)
end

--------------------------------------------------------------------------
-- A kernel.
--------------------------------------------------------------------------

--
-- **A kernel's text to its bytes**: lines, labels, comments. Two passes -
-- the first places every instruction and label, the second encodes, with
-- each branch's offset from itself to its label. Answers the bytes, or nil
-- and "line N: why".
--
function eu.assemble(text)
  local list, labels = {}, {}
  local n = 0

  for line in (text .. "\n"):gmatch("([^\n]*)\n") do
    n = n + 1

    local clean = line:gsub("//.*$", ""):match("^%s*(.-)%s*$")
    local label = clean:match("^([%a_][%w_]*):$")

    if label then
      if labels[label] then return nil, ("line %d: the label %s twice"):format(n, label) end
      labels[label] = #list * 16
    elseif clean ~= "" then
      local ok, ins = pcall(parse, clean)

      if not ok then
        return nil, ("line %d: %s"):format(n, type(ins) == "table" and ins.why or tostring(ins))
      end

      ins.line = n
      list[#list + 1] = ins
    end
  end

  local out = {}

  for k, ins in ipairs(list) do
    local at = (k - 1) * 16
    local offset = ins.target

    if ins.label then
      if not labels[ins.label] then
        return nil, ("line %d: no label %s"):format(ins.line, ins.label)
      end

      offset = labels[ins.label] - at
    end

    local ok, b = pcall(encode, ins, offset)

    if not ok then
      return nil, ("line %d: %s"):format(ins.line, type(b) == "table" and b.why or tostring(b))
    end

    out[#out + 1] = b
  end

  if #out == 0 then return nil, "no instructions" end

  return table.concat(out)
end

return eu
