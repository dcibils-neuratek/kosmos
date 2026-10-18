-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- **Kosmos's kernel language** (`docs/cafesa3d-gpu.md` R3, decision E1):
-- a kernel written C-shaped, compiled here to `eu.lua`'s assembly, which
-- `eu.lua` assembles and the GPU's driver checks as it checks any kernel -
-- so this compiler is never trusted with the GPU's safety. A mistake in it
-- is a refused kernel or a wrong picture, never a write anywhere else.
--
--   local kc = use("/Kosmos/Libraries/kc.lua")
--   local text, info = kc.compile(source, { simd = 8 })   -- eu.lua's text
--   local code, why = kc.assemble(source, { simd = 8 })   -- the bytes
--   local constants = kc.pack(info, { zoom = 2.5, most = 256 })
--
-- **The language**:
--
--   const float x0, dx;          // the run's constants, in this order
--   const int most;
--   buffer out;                  // binding-table entries, in this order
--
--   float square(float v) { return v * v; }
--
--   void main() {
--     int x = group_x * simd + lane;
--     float c = x0 + float(x) * dx;
--     int n = 0;
--     while (n < most && square(c) < 4.0) { n += 1; c = c * 0.5; }
--     store(out, x * 4, n);
--   }
--
-- `int`, `float`, `bool` and `vec3` (`.x`, `.y`, `.z`); the arithmetic,
-- the comparisons, `&&`, `||`, `!`, `? :`; `if`, `else`, `while`, `for`,
-- `break`, `continue`, `return`. Functions are inlined - the GPU has no
-- stack to call on - and may return early. Built in: `lane`, `group_x`,
-- `group_y`, `simd`; `sqrt`, `rsqrt`, `sin`, `cos`, `pow`, `exp2`, `log2`,
-- `floor`, `ceil`, `fract`, `abs`, `min`, `max`, `clamp`, `float`, `int`,
-- `vec3`, `dot`, `cross`, `length`, `normalize`; and the buffers' doors,
-- `load`, `loadi`, `load3`, `store` and `store3`, each by a byte offset.
--
-- **Every variable is a lane's own**: the program is one pixel's, or one
-- item's, and the compiler runs eight or sixteen of them at once without
-- saying so - a value is a register in SIMD8, two in SIMD16. The branches
-- are the GPU's structured ones, their targets written as the PRM says
-- (`tools/eu_emu.c` runs them the same way): an `if`'s failing lanes go to
-- the instruction after its `else`; a `break` or `continue` to the end of
-- the innermost block, then on.
--
-- Floating point is rounded as C rounds it - no fused multiply-adds - so a
-- kernel can be held to the same arithmetic on the processor. The GPU's
-- `math` - square roots, division, sines - is its own, close and not exact.

local eu = use and use("/Kosmos/Libraries/eu.lua") or dofile("user/lib/eu.lua")

local kc = {}

local function fail(line, why) error({ why = ("line %d: %s"):format(line or 0, why) }, 0) end

--------------------------------------------------------------------------
-- Words.
--------------------------------------------------------------------------

local PUNCT = { "<<=", ">>=", "&&", "||", "==", "!=", "<=", ">=", "<<", ">>", "+=", "-=", "*=", "/=",
                "(", ")", "{", "}", "[", "]", ",", ";", ".", "+", "-", "*", "/", "%", "&", "|",
                "^", "~", "!", "<", ">", "=", "?", ":" }

local function lex(src)
  local toks, i, line, n = {}, 1, 1, #src

  while i <= n do
    local c = src:sub(i, i)

    if c == "\n" then
      line = line + 1
      i = i + 1
    elseif c:match("%s") then
      i = i + 1
    elseif src:sub(i, i + 1) == "//" then
      i = (src:find("\n", i, true) or n + 1)
    elseif src:sub(i, i + 1) == "/*" then
      local e = src:find("*/", i + 2, true) or fail(line, "a comment that does not end")

      for _ in src:sub(i, e):gmatch("\n") do line = line + 1 end
      i = e + 2
    elseif c:match("[%a_]") then
      local w = src:match("^[%w_]+", i)

      toks[#toks + 1] = { k = "id", v = w, line = line }
      i = i + #w
    elseif c:match("%d") or (c == "." and src:sub(i + 1, i + 1):match("%d")) then
      local num = src:match("^%d*%.?%d*[eE][%+%-]?%d+", i) or src:match("^0[xX]%x+", i)
                  or src:match("^%d*%.%d*", i) or src:match("^%d+", i)
      local isf = num:find("[%.eE]") and not num:match("^0[xX]")

      i = i + #num

      if src:sub(i, i) == "f" then i = i + 1; isf = true end

      toks[#toks + 1] = { k = isf and "float" or "int", v = tonumber(num), line = line }
    else
      local p

      for _, q in ipairs(PUNCT) do
        if src:sub(i, i + #q - 1) == q then p = q; break end
      end

      if not p then fail(line, "a character the language does not have: " .. c) end

      toks[#toks + 1] = { k = "p", v = p, line = line }
      i = i + #p
    end
  end

  toks[#toks + 1] = { k = "eof", v = "", line = line }
  return toks
end

--------------------------------------------------------------------------
-- The parse: a program of constants, buffers and functions.
--------------------------------------------------------------------------

local TYPES = { int = "d", float = "f", bool = "b", vec3 = "v", void = "void" }

local function parser(toks)
  local P, at = {}, 1

  local function peek(k) return toks[at + (k or 0)] end
  local function next_tok() local t = toks[at]; at = at + 1; return t end
  local function is(v) return toks[at].k == "p" and toks[at].v == v end
  local function is_id(v) return toks[at].k == "id" and toks[at].v == v end

  local function expect(v)
    local t = next_tok()

    if t.v ~= v or (t.k ~= "p" and t.k ~= "id") then
      fail(t.line, ("%s expected, not %s"):format(v, t.v ~= "" and t.v or "the end"))
    end

    return t
  end

  local function name()
    local t = next_tok()

    if t.k ~= "id" then fail(t.line, "a name expected, not " .. tostring(t.v)) end

    return t.v, t.line
  end

  -- Expressions, by precedence.
  local expr

  local function primary()
    local t = next_tok()

    if t.k == "int" or t.k == "float" then
      return { e = "num", t = t.k == "int" and "d" or "f", v = t.v, line = t.line }
    end

    if t.k == "id" and (t.v == "true" or t.v == "false") then
      return { e = "num", t = "b", v = t.v == "true" and -1 or 0, line = t.line }
    end

    if t.k == "id" then
      if is("(") then
        next_tok()
        local args = {}

        if not is(")") then
          repeat
            args[#args + 1] = expr()
          until not (is(",") and next_tok())
        end

        expect(")")
        return { e = "call", name = t.v, args = args, line = t.line }
      end

      return { e = "var", name = t.v, line = t.line }
    end

    if t.k == "p" and t.v == "(" then
      local e = expr()

      expect(")")
      return e
    end

    fail(t.line, "an expression expected, not " .. tostring(t.v))
  end

  local function postfix()
    local e = primary()

    while is(".") do
      next_tok()
      local c, line = name()

      if c ~= "x" and c ~= "y" and c ~= "z" then fail(line, "a vec3 has .x, .y and .z") end

      e = { e = "comp", of = e, c = c, line = line }
    end

    return e
  end

  local function unary()
    if is("-") or is("!") or is("~") then
      local t = next_tok()

      return { e = "un", op = t.v, a = unary(), line = t.line }
    end

    return postfix()
  end

  local LEVELS = {
    { "||" }, { "&&" }, { "|" }, { "^" }, { "&" }, { "==", "!=" }, { "<", "<=", ">", ">=" },
    { "<<", ">>" }, { "+", "-" }, { "*", "/", "%" },
  }

  local function binary(level)
    if level > #LEVELS then return unary() end

    local a = binary(level + 1)

    while true do
      local t, found = peek(), false

      if t.k == "p" then
        for _, op in ipairs(LEVELS[level]) do
          if t.v == op then found = true break end
        end
      end

      if not found then return a end

      next_tok()
      a = { e = "bin", op = t.v, a = a, b = binary(level + 1), line = t.line }
    end
  end

  function expr()
    local c = binary(1)

    if is("?") then
      local t = next_tok()
      local a = expr()

      expect(":")
      return { e = "sel", c = c, a = a, b = expr(), line = t.line }
    end

    return c
  end

  -- Statements.
  local statement

  local function block()
    local t = expect("{")
    local list = { s = "block", line = t.line }

    while not is("}") do
      if peek().k == "eof" then fail(t.line, "a block that does not end") end
      list[#list + 1] = statement()
    end

    expect("}")
    return list
  end

  local function decl(ty, line)
    local d = { s = "decl", t = ty, line = line }

    repeat
      local n = name()

      d[#d + 1] = { name = n, init = is("=") and (next_tok() and expr()) or nil }
    until not (is(",") and next_tok())

    return d
  end

  local function simple()
    local t = peek()

    if t.k == "id" and TYPES[t.v] and t.v ~= "void" then
      next_tok()
      return decl(TYPES[t.v], t.line)
    end

    local target = expr()

    if is("=") or is("+=") or is("-=") or is("*=") or is("/=") then
      local op = next_tok().v

      if target.e ~= "var" and target.e ~= "comp" then fail(t.line, "only a variable can be assigned to") end

      return { s = "set", target = target, op = op, v = expr(), line = t.line }
    end

    return { s = "expr", v = target, line = t.line }
  end

  function statement()
    local t = peek()

    if t.k == "p" and t.v == "{" then return block() end

    if t.k == "id" then
      if t.v == "if" then
        next_tok(); expect("(")
        local c = expr()
        expect(")")
        local s = { s = "if", c = c, a = statement(), line = t.line }

        if is_id("else") then next_tok(); s.b = statement() end
        return s
      elseif t.v == "while" then
        next_tok(); expect("(")
        local c = expr()
        expect(")")
        return { s = "while", c = c, body = statement(), line = t.line }
      elseif t.v == "for" then
        next_tok(); expect("(")
        local init = not is(";") and simple() or nil
        expect(";")
        local c = not is(";") and expr() or nil
        expect(";")
        local step = not is(")") and simple() or nil
        expect(")")
        return { s = "for", init = init, c = c, step = step, body = statement(), line = t.line }
      elseif t.v == "break" or t.v == "continue" then
        next_tok(); expect(";")
        return { s = t.v, line = t.line }
      elseif t.v == "return" then
        next_tok()
        local v = not is(";") and expr() or nil
        expect(";")
        return { s = "return", v = v, line = t.line }
      end
    end

    local s = simple()

    expect(";")
    return s
  end

  -- The program.
  function P.program()
    local prog = { consts = {}, buffers = {}, funcs = {} }

    while peek().k ~= "eof" do
      local t = next_tok()

      if t.k ~= "id" then fail(t.line, "a declaration expected, not " .. tostring(t.v)) end

      if t.v == "const" then
        local ty = next_tok()

        if not TYPES[ty.v] or ty.v == "void" or ty.v == "bool" then fail(ty.line, "a constant is int, float or vec3") end

        repeat
          local n, line = name()
          prog.consts[#prog.consts + 1] = { name = n, t = TYPES[ty.v], line = line }
        until not (is(",") and next_tok())

        expect(";")
      elseif t.v == "buffer" then
        repeat
          local n, line = name()
          prog.buffers[#prog.buffers + 1] = { name = n, line = line }
        until not (is(",") and next_tok())

        expect(";")
      elseif TYPES[t.v] then
        local fname, line = name()
        local params = {}

        expect("(")

        if not is(")") then
          repeat
            local pt = next_tok()

            if not TYPES[pt.v] or pt.v == "void" then fail(pt.line, "a parameter's type expected") end

            params[#params + 1] = { t = TYPES[pt.v], name = (name()) }
          until not (is(",") and next_tok())
        end

        expect(")")

        if prog.funcs[fname] then fail(line, "the function " .. fname .. " twice") end

        prog.funcs[fname] = { name = fname, ret = TYPES[t.v], params = params, body = block(), line = line }
      else
        fail(t.line, "a constant, a buffer or a function expected, not " .. t.v)
      end
    end

    if not prog.funcs.main then fail(1, "no main") end

    return prog
  end

  return P
end

--------------------------------------------------------------------------
-- The code.
--------------------------------------------------------------------------

local CONST_REG = 1               -- r1 onwards: the run's constants
local LAST_REG = 111              -- r112 onwards: the thread's end

local function f32_hex(x)
  return ("0x%08x:f"):format(string.unpack("<I4", string.pack("<f", x)))
end

local function gen(prog, simd)
  local G = { out = {}, label = 0 }
  local W = simd
  local R = W // 8
  local EX = ("(%d|M0)"):format(W)
  local used = {}
  local scopes, blocks, fnstack = {}, {}, {}
  local temps
  local line_now = 0

  local function emit(s) G.out[#G.out + 1] = s end
  local function label() G.label = G.label + 1; return "L" .. G.label end
  local function place(l) emit(l .. ":") end

  -- The constants' places: four bytes a scalar, twelve a vec3.
  local layout, offset = {}, 0

  for _, c in ipairs(prog.consts) do
    layout[c.name] = { t = c.t, off = offset, line = c.line }
    offset = offset + (c.t == "v" and 12 or 4)
  end

  if offset > 256 then fail(1, "more than 256 bytes of constants") end

  local first_free = CONST_REG + math.max(1, (offset + 31) // 32)

  for r = 0, first_free - 1 do used[r] = true end

  -- Registers: a value takes R of them, aligned.
  local function alloc(count)
    count = count or R
    for r = first_free, LAST_REG - count + 1 do
      if r % R == 0 then
        local free = true

        for k = r, r + count - 1 do
          if used[k] then free = false break end
        end

        if free then
          for k = r, r + count - 1 do used[k] = true end
          return r
        end
      end
    end

    fail(line_now, "the kernel needs more registers than the GPU's thread has")
  end

  local function release(r, count)
    for k = r, r + (count or R) - 1 do used[k] = nil end
  end

  local function temp(t)
    local r = alloc()

    temps[#temps + 1] = r
    return { k = "reg", t = t, r = r }
  end

  -- Operands as eu.lua reads them.
  local function src(v, neg)
    local m = neg and "-" or ""

    if v.k == "reg" then return ("%sr%d.0<8;8,1>:%s"):format(m, v.r, v.t == "b" and "d" or v.t) end
    if v.k == "uni" then return ("%sr%d.%d<0;1,0>:%s"):format(m, v.r, v.s, v.t) end

    if v.k == "imm" then
      local x = neg and -v.v or v.v

      if v.t == "f" then return f32_hex(x) end

      return ("%d:d"):format(math.tointeger(x) or fail(line_now, "an integer constant that is not one"))
    end

    fail(line_now, "a value that is not a number")
  end

  local function dst(v) return ("r%d.0<1>:%s"):format(v.r, v.t == "b" and "d" or v.t) end

  local function scalar_type(v)
    if v.k == "vec" then fail(line_now, "a vec3 where a number is wanted") end
    return v.t
  end

  -- A value of type `t`: converted, or a constant made one.
  local function as(v, t)
    if v.k == "vec" then
      if t == "v" then return v end
      fail(line_now, "a vec3 where a number is wanted")
    end

    if t == "v" then
      local s = as(v, "f")

      return { k = "vec", x = s, y = s, z = s }
    end

    local vt = v.t == "b" and "d" or v.t
    local want = t == "b" and "d" or t

    if vt == want then
      if t == "b" and v.t ~= "b" then
        -- An int as a condition: true when not nought.
        local r = temp("b")
        emit(("cmp %s (ne)f1.0 %s %s 0:d"):format(EX, dst(r), src(v)))
        return r
      end
      return v
    end

    if v.k == "imm" then
      if want == "f" then return { k = "imm", t = "f", v = v.v + 0.0 } end
      return { k = "imm", t = "d", v = math.tointeger(v.v >= 0 and math.floor(v.v) or math.ceil(v.v)) }
    end

    local r = temp(want)
    emit(("mov %s %s %s"):format(EX, dst(r), src(v)))
    if t == "b" then return as(r, "b") end
    return r
  end

  -- A value in a register: what the first source of an instruction must be.
  local function reg(v)
    if v.k == "reg" or v.k == "uni" then return v end

    local r = temp(v.t == "b" and "d" or v.t)

    emit(("mov %s %s %s"):format(EX, dst(r), src(v)))
    return r
  end

  local function full_reg(v)
    if v.k == "reg" then return v end

    local r = temp(v.t == "b" and "d" or v.t)

    emit(("mov %s %s %s"):format(EX, dst(r), src(v)))
    return r
  end

  -- Names.
  local function lookup(n, line)
    for i = #scopes, 1, -1 do
      if scopes[i].vars[n] then return scopes[i].vars[n] end
    end

    if layout[n] then
      local l = layout[n]
      local function at(off) return { k = "uni", t = "f", r = CONST_REG + off // 32, s = (off % 32) // 4 } end

      if l.t == "v" then
        return { k = "vec", x = at(l.off), y = at(l.off + 4), z = at(l.off + 8) }
      end

      local u = at(l.off)
      u.t = l.t
      return u
    end

    if n == "simd" then return { k = "imm", t = "d", v = W } end

    fail(line, "no variable " .. n)
  end

  local function new_var(n, t, line)
    local scope = scopes[#scopes]

    if scope.vars[n] then fail(line, "the variable " .. n .. " twice") end

    local v

    if t == "v" then
      v = { k = "vec", x = { k = "reg", t = "f", r = alloc() }, y = { k = "reg", t = "f", r = alloc() },
            z = { k = "reg", t = "f", r = alloc() } }
      scope.regs[#scope.regs + 1] = v.x.r
      scope.regs[#scope.regs + 1] = v.y.r
      scope.regs[#scope.regs + 1] = v.z.r
    else
      v = { k = "reg", t = t, r = alloc() }
      scope.regs[#scope.regs + 1] = v.r
    end

    scope.vars[n] = v
    return v
  end

  local function push_scope() scopes[#scopes + 1] = { vars = {}, regs = {} } end

  local function pop_scope()
    local s = table.remove(scopes)

    for _, r in ipairs(s.regs) do release(r) end
  end

  local function move(to, v)
    if to.k == "vec" then
      v = as(v, "v")
      emit(("mov %s %s %s"):format(EX, dst(to.x), src(as(v.x, "f"))))
      emit(("mov %s %s %s"):format(EX, dst(to.y), src(as(v.y, "f"))))
      emit(("mov %s %s %s"):format(EX, dst(to.z), src(as(v.z, "f"))))
      return
    end

    emit(("mov %s %s %s"):format(EX, dst(to), src(as(v, to.t))))
  end

  -- Arithmetic.
  local eval

  local OPS = { ["+"] = "add", ["*"] = "mul", ["&"] = "and", ["|"] = "or", ["^"] = "xor",
                ["<<"] = "shl", [">>"] = "asr" }
  local COMMUTES = { add = true, mul = true, ["and"] = true, ["or"] = true, xor = true }
  local CONDS = { ["<"] = "lt", ["<="] = "le", [">"] = "gt", [">="] = "ge", ["=="] = "eq", ["!="] = "ne" }
  local SWAPPED = { lt = "gt", le = "ge", gt = "lt", ge = "le", eq = "eq", ne = "ne" }

  local function fold(op, a, b, t)
    local x, y = a.v, b.v
    local r

    if op == "+" then r = x + y
    elseif op == "-" then r = x - y
    elseif op == "*" then r = x * y
    elseif op == "/" then
      if t ~= "f" then return nil end
      r = x / y
    else return nil end

    if t == "f" then r = string.unpack("<f", string.pack("<f", r)) end

    return { k = "imm", t = t, v = t == "f" and r or math.tointeger(r) }
  end

  local function arith(op, a, b, line)
    if a.k == "vec" or b.k == "vec" then
      if op ~= "+" and op ~= "-" and op ~= "*" and op ~= "/" then fail(line, op .. " on a vec3") end

      a, b = as(a, "v"), as(b, "v")

      return { k = "vec", x = arith(op, a.x, b.x, line), y = arith(op, a.y, b.y, line),
               z = arith(op, a.z, b.z, line) }
    end

    local t = (scalar_type(a) == "f" or scalar_type(b) == "f") and "f" or "d"

    if op == "&" or op == "|" or op == "^" or op == "<<" or op == ">>" or op == "%" then
      if t == "f" then fail(line, op .. " on a float") end
    end

    a, b = as(a, t), as(b, t)

    if a.k == "imm" and b.k == "imm" then
      local f = fold(op, a, b, t)
      if f then return f end
    end

    if op == "/" then
      if t == "d" then
        local p = b.k == "imm" and b.v > 0 and math.tointeger(math.log(b.v, 2))

        if p and 2 ^ p == b.v then return arith(">>", a, { k = "imm", t = "d", v = p }, line) end
        fail(line, "integer division by anything but a power of two; make it float")
      end

      local r = temp("f")
      emit(("math.fdiv %s %s %s %s"):format(EX, dst(r), src(reg(a)), src(full_reg(b))))
      return r
    end

    if op == "%" then
      local p = b.k == "imm" and b.v > 0 and math.tointeger(math.log(b.v, 2))

      if not (p and 2 ^ p == b.v) then fail(line, "% by anything but a power of two") end
      return arith("&", a, { k = "imm", t = "d", v = b.v - 1 }, line)
    end

    local name, neg = OPS[op], false

    if op == "-" then name, neg = "add", true end

    -- A constant cannot be the first source: swapped where the order does
    -- not matter, put in a register where it does - `5 - x` among them.
    if a.k == "imm" then
      if COMMUTES[name] and not neg then a, b = b, a
      else a = reg(a) end
    end

    local r = temp(t)

    emit(("%s %s %s %s %s"):format(name, EX, dst(r), src(a), src(b, neg)))
    return r
  end

  local function compare(op, a, b, line)
    local t = (scalar_type(a) == "f" or scalar_type(b) == "f") and "f" or "d"
    local cond = CONDS[op]

    a, b = as(a, t), as(b, t)

    if a.k == "imm" then a, b, cond = b, a, SWAPPED[cond] end

    a = reg(a)

    -- The destination of the sources' type: all ones or nought, read as an int.
    local r = temp(t)

    emit(("cmp %s (%s)f1.0 %s %s %s"):format(EX, cond, dst(r), src(a), src(b)))
    return { k = "reg", t = "b", r = r.r }
  end

  local function logic(op, a, b)
    a, b = as(a, "b"), as(b, "b")

    local r = temp("b")

    emit(("%s %s %s %s %s"):format(op == "&&" and "and" or "or", EX, dst(r), src(reg(a)), src(b)))
    return r
  end

  local function select(c, a, b)
    if a.k == "vec" or b.k == "vec" then
      a, b = as(a, "v"), as(b, "v")
      return { k = "vec", x = select(c, a.x, b.x), y = select(c, a.y, b.y), z = select(c, a.z, b.z) }
    end

    local t = (a.t == "f" or b.t == "f") and "f" or (a.t == "b" and b.t == "b" and "b" or "d")

    a, b = as(a, t), as(b, t)
    a = reg(a)

    local r = temp(t)

    emit(("cmp %s (ne)f1.0 null<1>:d %s 0:d"):format(EX, src(reg(c))))
    emit(("(f1.0) sel %s %s %s %s"):format(EX, dst(r), src(a), src(b)))
    return r
  end

  local function minmax(which, a, b, line)
    if a.k == "vec" or b.k == "vec" then
      a, b = as(a, "v"), as(b, "v")
      return { k = "vec", x = minmax(which, a.x, b.x, line), y = minmax(which, a.y, b.y, line),
               z = minmax(which, a.z, b.z, line) }
    end

    local t = (scalar_type(a) == "f" or scalar_type(b) == "f") and "f" or "d"

    a, b = reg(as(a, t)), as(b, t)

    local r = temp(t)

    emit(("cmp %s (%s)f1.0 null<1>:%s %s %s"):format(EX, which == "min" and "lt" or "gt", t, src(a), src(b)))
    emit(("(f1.0) sel %s %s %s %s"):format(EX, dst(r), src(a), src(b)))
    return r
  end

  local MATH = { sqrt = "sqt", rsqrt = "rsq", sin = "sin", cos = "cos", exp2 = "exp", log2 = "log" }
  local ROUND = { floor = "rndd", ceil = "rndu", fract = "frc" }

  local function math1(fn, v)
    local r = temp("f")

    emit(("math.%s %s %s %s null<8;8,1>:f"):format(fn, EX, dst(r), src(full_reg(as(v, "f")))))
    return r
  end

  local function dot(a, b, line)
    a, b = as(a, "v"), as(b, "v")

    return arith("+", arith("+", arith("*", a.x, b.x, line), arith("*", a.y, b.y, line), line),
                 arith("*", a.z, b.z, line), line)
  end

  -- The data port: untyped reads and writes, by a byte offset.
  local function buffer_index(e, line)
    if e.e ~= "var" then fail(line, "a buffer by its name") end

    for i, b in ipairs(prog.buffers) do
      if b.name == e.name then return i - 1 end
    end

    fail(line, "no buffer " .. e.name)
  end

  local function desc(type_, channels, mlen, rlen, bti)
    local cmask = 0xf & (0xf << channels)
    local mode = W == 16 and 1 or 2

    return (mlen << 25) | (rlen << 20) | (type_ << 14) | (((mode << 4) | cmask) << 8) | bti
  end

  local function load(e, n, t, line)
    local bti = buffer_index(e.args[1], line)
    local addr = alloc(R)
    local back = alloc(R * n)

    temps[#temps + 1] = addr
    for k = 0, n - 1 do temps[#temps + 1] = back + k * R end

    emit(("mov %s r%d.0<1>:d %s"):format(EX, addr, src(as(eval(e.args[2]), "d"))))
    emit(("send.hdc1 %s r%d r%d 0x00000000 0x%08X"):format(EX, back, addr, desc(1, n, R, R * n, bti)))

    if n == 1 then return { k = "reg", t = t, r = back } end

    return { k = "vec", x = { k = "reg", t = "f", r = back }, y = { k = "reg", t = "f", r = back + R },
             z = { k = "reg", t = "f", r = back + 2 * R } }
  end

  local function store(e, n, line)
    local bti = buffer_index(e.args[1], line)
    local block = alloc(R * (1 + n))
    local v = eval(e.args[3])

    for k = 0, n do temps[#temps + 1] = block + k * R end

    emit(("mov %s r%d.0<1>:d %s"):format(EX, block, src(as(eval(e.args[2]), "d"))))

    if n == 1 then
      local s = scalar_type(v) == "b" and as(v, "d") or v
      emit(("mov %s r%d.0<1>:%s %s"):format(EX, block + R, s.t, src(s)))
    else
      v = as(v, "v")
      emit(("mov %s r%d.0<1>:f %s"):format(EX, block + R, src(as(v.x, "f"))))
      emit(("mov %s r%d.0<1>:f %s"):format(EX, block + 2 * R, src(as(v.y, "f"))))
      emit(("mov %s r%d.0<1>:f %s"):format(EX, block + 3 * R, src(as(v.z, "f"))))
    end

    emit(("send.hdc1 %s null r%d 0x00000000 0x%08X"):format(EX, block, desc(9, n, R * (1 + n), 0, bti)))
    return { k = "imm", t = "d", v = 0 }
  end

  local inline

  local function call(e)
    local n, args, line = e.name, e.args, e.line
    local function want(k)
      if #args ~= k then fail(line, ("%s takes %d"):format(n, k)) end
    end

    if MATH[n] then want(1) return math1(MATH[n], eval(args[1])) end

    if ROUND[n] then
      want(1)
      local r = temp("f")
      emit(("%s %s %s %s"):format(ROUND[n], EX, dst(r), src(full_reg(as(eval(args[1]), "f")))))
      return r
    end

    if n == "pow" then
      want(2)
      local r = temp("f")
      local a, b = full_reg(as(eval(args[1]), "f")), full_reg(as(eval(args[2]), "f"))
      emit(("math.pow %s %s %s %s"):format(EX, dst(r), src(a), src(b)))
      return r
    end

    if n == "abs" then
      want(1)
      local v = eval(args[1])

      if v.k == "vec" then fail(line, "abs of a vec3") end
      if v.k == "imm" then return { k = "imm", t = v.t, v = math.abs(v.v) } end

      local r = temp(v.t == "b" and "d" or v.t)
      emit(("mov %s %s (abs)%s"):format(EX, dst(r), src(v)))
      return r
    end

    if n == "min" or n == "max" then want(2) return minmax(n, eval(args[1]), eval(args[2]), line) end

    if n == "clamp" then
      want(3)
      return minmax("min", minmax("max", eval(args[1]), eval(args[2]), line), eval(args[3]), line)
    end

    if n == "float" then want(1) return as(eval(args[1]), "f") end
    if n == "int" then want(1) return as(eval(args[1]), "d") end

    if n == "vec3" then
      want(3)
      return { k = "vec", x = as(eval(args[1]), "f"), y = as(eval(args[2]), "f"), z = as(eval(args[3]), "f") }
    end

    if n == "dot" then want(2) return dot(eval(args[1]), eval(args[2]), line) end
    if n == "length" then want(1) local v = eval(args[1]) return math1("sqt", dot(v, v, line)) end

    if n == "cross" then
      want(2)
      local a, b = as(eval(args[1]), "v"), as(eval(args[2]), "v")
      local function m(p, q) return arith("*", p, q, line) end

      return { k = "vec", x = arith("-", m(a.y, b.z), m(a.z, b.y), line),
               y = arith("-", m(a.z, b.x), m(a.x, b.z), line),
               z = arith("-", m(a.x, b.y), m(a.y, b.x), line) }
    end

    if n == "normalize" then
      want(1)
      -- As the 3D Kit's `norm`: a times 1/l where the length is more than nought.
      local a = as(eval(args[1]), "v")
      local l = math1("sqt", dot(a, a, line))
      local k = math1("inv", l)
      local scaled = arith("*", a, k, line)

      return select(compare(">", l, { k = "imm", t = "f", v = 0.0 }, line), scaled, a)
    end

    if n == "load" then want(2) return load(e, 1, "f", line) end
    if n == "loadi" then want(2) return load(e, 1, "d", line) end
    if n == "load3" then want(2) return load(e, 3, "f", line) end
    if n == "store" then want(3) return store(e, 1, line) end
    if n == "store3" then want(3) return store(e, 3, line) end

    if prog.funcs[n] then return inline(prog.funcs[n], args, line) end

    fail(line, "no function " .. n)
  end

  function eval(e)
    line_now = e.line or line_now

    if e.e == "num" then return { k = "imm", t = e.t, v = e.v } end

    if e.e == "var" then
      if e.name == "lane" or e.name == "group_x" or e.name == "group_y" then return G.builtin[e.name] end
      return lookup(e.name, e.line)
    end

    if e.e == "comp" then
      local v = eval(e.of)

      if v.k ~= "vec" then fail(e.line, "." .. e.c .. " of a number") end
      return v[e.c]
    end

    if e.e == "call" then return call(e) end

    if e.e == "un" then
      local a = eval(e.a)

      if e.op == "-" then
        if a.k == "vec" then
          return { k = "vec", x = arith("-", { k = "imm", t = "f", v = 0.0 }, a.x, e.line),
                   y = arith("-", { k = "imm", t = "f", v = 0.0 }, a.y, e.line),
                   z = arith("-", { k = "imm", t = "f", v = 0.0 }, a.z, e.line) }
        end

        if a.k == "imm" then return { k = "imm", t = a.t, v = -a.v } end

        local r = temp(a.t == "b" and "d" or a.t)
        emit(("mov %s %s %s"):format(EX, dst(r), src(a, true)))
        return r
      end

      if e.op == "!" then
        local b = as(a, "b")
        local r = temp("b")
        emit(("not %s %s %s"):format(EX, dst(r), src(reg(b))))
        return r
      end

      local r = temp("d")
      emit(("not %s %s %s"):format(EX, dst(r), src(reg(as(a, "d")))))
      return r
    end

    if e.e == "bin" then
      if CONDS[e.op] then return compare(e.op, eval(e.a), eval(e.b), e.line) end
      if e.op == "&&" or e.op == "||" then return logic(e.op, eval(e.a), eval(e.b)) end
      return arith(e.op, eval(e.a), eval(e.b), e.line)
    end

    if e.e == "sel" then
      local c = as(eval(e.c), "b")
      return select(c, eval(e.a), eval(e.b))
    end

    fail(e.line, "an expression the compiler does not know")
  end

  -- Statements, and the GPU's structured branches.
  local stmt

  -- The end of the innermost block: where a lane that leaves goes first.
  local function innermost_end()
    return blocks[#blocks] and blocks[#blocks].endl
  end

  local function innermost_loop()
    for i = #blocks, 1, -1 do
      if blocks[i].kind == "loop" then return blocks[i] end
    end
  end

  local function statement_temps(f)
    local saved = temps

    temps = {}
    f()

    for _, r in ipairs(temps) do release(r) end
    temps = saved
  end

  local function condition_flag(c, line, flag_true)
    -- Evaluated, then into f0.0: set where the lane takes the branch.
    statement_temps(function()
      local b = as(eval(c), "b")
      emit(("cmp %s (%s)f0.0 null<1>:d %s 0:d"):format(EX, flag_true and "ne" or "eq", src(reg(b))))
    end)
  end

  local function body(s)
    push_scope()
    stmt(s)
    pop_scope()
  end

  local function loop(cond, step, body_s, line)
    local top, endl, after = label(), label(), label()
    local ctx = { kind = "loop", endl = endl, after = after, step = step, line = line }

    place(top)

    if cond then
      condition_flag(cond, line, false)
      emit(("(f0.0) break %s jip: %s uip: %s"):format(EX, endl, after))
    end

    blocks[#blocks + 1] = ctx
    body(body_s)

    if step then
      push_scope()
      stmt(step)
      pop_scope()
    end

    table.remove(blocks)
    place(endl)
    emit(("while %s jip: %s"):format(EX, top))
    place(after)
  end

  local function leave(kind, line)
    local l = innermost_loop()

    if not l or l.fn then fail(line, kind .. " outside a loop") end

    if kind == "continue" and l.step then
      push_scope()
      stmt(l.step)
      pop_scope()
    end

    emit(("%s %s jip: %s uip: %s"):format(kind, EX, innermost_end(), kind == "break" and l.after or l.endl))
  end

  function stmt(s)
    line_now = s.line or line_now

    if s.s == "block" then
      push_scope()
      for _, x in ipairs(s) do stmt(x) end
      pop_scope()
    elseif s.s == "decl" then
      for _, d in ipairs(s) do
        local v

        if d.init then
          statement_temps(function()
            local init = eval(d.init)
            v = new_var(d.name, s.t == "b" and "b" or s.t, s.line)
            move(v, init)
          end)
        else
          v = new_var(d.name, s.t, s.line)
        end
      end
    elseif s.s == "set" then
      statement_temps(function()
        local target = eval(s.target)
        local v = eval(s.v)

        if s.op ~= "=" then v = arith(s.op:sub(1, 1), target, v, s.line) end
        if target.k == "uni" or target.k == "imm" then fail(s.line, "a constant cannot be assigned to") end

        move(target, v)
      end)
    elseif s.s == "expr" then
      statement_temps(function() eval(s.v) end)
    elseif s.s == "if" then
      local endl, other = label(), s.b and label() or nil
      local outer = innermost_end()
      local after = label()

      condition_flag(s.c, s.line, true)
      emit(("(f0.0) if %s jip: %s uip: %s"):format(EX, other or endl, endl))

      blocks[#blocks + 1] = { kind = "if", endl = endl }
      body(s.a)

      if s.b then
        emit(("else %s jip: %s uip: %s"):format(EX, endl, endl))
        place(other)
        body(s.b)
      end

      table.remove(blocks)
      place(endl)
      emit(("endif %s jip: %s"):format(EX, outer or after))
      place(after)
    elseif s.s == "while" then
      loop(s.c, nil, s.body, s.line)
    elseif s.s == "for" then
      push_scope()
      if s.init then stmt(s.init) end
      loop(s.c, s.step, s.body, s.line)
      pop_scope()
    elseif s.s == "break" or s.s == "continue" then
      leave(s.s, s.line)
    elseif s.s == "return" then
      local f = fnstack[#fnstack]

      if s.v then
        if f.ret == "void" then fail(s.line, "a void function returning a value") end
        statement_temps(function() move(f.result, eval(s.v)) end)
      elseif f.ret ~= "void" then
        fail(s.line, "return with no value")
      end

      if f.once then
        local l = innermost_loop()

        if l ~= f.once then fail(s.line, "return from inside a loop; keep a flag and break") end
        emit(("break %s jip: %s uip: %s"):format(EX, innermost_end(), f.once.after))
      end
    else
      fail(s.line, "a statement the compiler does not know")
    end
  end

  -- A function, inlined: its parameters copies, its early returns a break
  -- out of a loop that runs once.
  local depth = 0

  local function returns_early(b)
    local early = false

    local function walk(s, last)
      if type(s) ~= "table" then return end
      if s.s == "return" and not last then early = true end
      if s.s == "block" then
        for i, x in ipairs(s) do walk(x, last and i == #s) end
      elseif s.s == "if" then
        walk(s.a, false); walk(s.b, false)
      elseif s.s == "while" or s.s == "for" then
        walk(s.body, false)
      end
    end

    walk(b, true)
    return early
  end

  function inline(f, args, line)
    if #args ~= #f.params then fail(line, ("%s takes %d"):format(f.name, #f.params)) end

    depth = depth + 1
    if depth > 32 then fail(line, "a function that calls itself") end

    local values = {}

    for i, a in ipairs(args) do values[i] = eval(a) end

    -- The result outlives the call: a register of the caller's statement.
    local result

    if f.ret == "v" then
      result = { k = "vec", x = temp("f"), y = temp("f"), z = temp("f") }
    elseif f.ret ~= "void" then
      result = temp(f.ret)
    end

    local saved_scopes = scopes
    local saved_temps = temps

    scopes = { scopes[1] }                                -- the program's, and the function's own
    push_scope()

    for i, p in ipairs(f.params) do
      local v = new_var(p.name, p.t, line)
      move(v, values[i])
    end

    local ctx = { ret = f.ret, result = result }
    fnstack[#fnstack + 1] = ctx

    if returns_early(f.body) then
      local top, endl, after = label(), label(), label()

      ctx.once = { kind = "loop", fn = true, endl = endl, after = after }
      place(top)
      blocks[#blocks + 1] = ctx.once
      temps = {}
      stmt(f.body)
      for _, r in ipairs(temps) do release(r) end
      table.remove(blocks)
      place(endl)
      emit(("cmp %s (ne)f1.1 null<1>:d r0.0<0;1,0>:d r0.0<0;1,0>:d"):format(EX))
      emit(("(f1.1) while %s jip: %s"):format(EX, top))
      place(after)
    else
      temps = {}
      stmt(f.body)
      for _, r in ipairs(temps) do release(r) end
    end

    table.remove(fnstack)
    pop_scope()
    scopes = saved_scopes
    temps = saved_temps
    depth = depth - 1

    return result or { k = "imm", t = "d", v = 0 }
  end

  -- The kernel: the lanes and the group, main, and the thread's end.
  push_scope()
  temps = {}

  local lanes = alloc(1)
  G.builtin = { lane = { k = "reg", t = "d", r = alloc() }, group_x = { k = "reg", t = "d", r = alloc() },
                group_y = { k = "reg", t = "d", r = alloc() } }

  emit(("(W) mov (8|M0) r%d.0<1>:uw 0x76543210:v"):format(lanes))
  if W == 16 then emit(("(W) add (8|M0) r%d.8<1>:uw r%d.0<8;8,1>:uw 8:uw"):format(lanes, lanes)) end
  emit(("mov %s %s r%d.0<8;8,1>:uw"):format(EX, dst(G.builtin.lane), lanes))
  emit(("mov %s %s r0.1<0;1,0>:ud"):format(EX, dst(G.builtin.group_x)))
  emit(("mov %s %s r0.6<0;1,0>:ud"):format(EX, dst(G.builtin.group_y)))
  release(lanes, 1)

  if prog.funcs.main.ret ~= "void" or #prog.funcs.main.params > 0 then
    fail(prog.funcs.main.line, "main is void main()")
  end

  inline(prog.funcs.main, {}, prog.funcs.main.line)

  emit("(W) mov (8|M0) r112.0<1>:ud r0.0<8;8,1>:ud")
  emit(("(W) send.ts %s null r112:d 0x00000000 0x02000010 {EOT}"):format(EX))

  local info = { simd = W, constants = {}, buffers = {}, constant_bytes = offset }

  for _, c in ipairs(prog.consts) do
    info.constants[#info.constants + 1] = { name = c.name, t = c.t, off = layout[c.name].off }
  end

  for _, b in ipairs(prog.buffers) do info.buffers[#info.buffers + 1] = b.name end

  return table.concat(G.out, "\n") .. "\n", info
end

--------------------------------------------------------------------------
-- The doors.
--------------------------------------------------------------------------

function kc.compile(src, opts)
  local simd = opts and opts.simd or 8

  if simd ~= 8 and simd ~= 16 then return nil, "a kernel is SIMD8 or SIMD16" end

  local ok, text, info = pcall(function()
    local prog = parser(lex(src)).program()
    return gen(prog, simd)
  end)

  if not ok then return nil, type(text) == "table" and text.why or tostring(text) end

  return text, info
end

function kc.assemble(src, opts)
  local text, info = kc.compile(src, opts)

  if not text then return nil, info end

  local code, lines = eu.assemble(text)

  if not code then return nil, "the compiler's assembly: " .. tostring(lines) end

  info.text = text
  return code, info
end

-- The constants as the run takes them: `values` by name, in the layout
-- `compile` worked out.
function kc.pack(info, values)
  local parts = {}

  for _, c in ipairs(info.constants) do
    local v = values[c.name]

    if v == nil then error("no value for the constant " .. c.name, 2) end

    if c.t == "v" then
      parts[#parts + 1] = string.pack("<fff", v[1] or v.x, v[2] or v.y, v[3] or v.z)
    elseif c.t == "f" then
      parts[#parts + 1] = string.pack("<f", v)
    else
      parts[#parts + 1] = string.pack("<i4", math.tointeger(v) or error("the constant " .. c.name .. " is an int", 2))
    end
  end

  return table.concat(parts)
end

return kc
