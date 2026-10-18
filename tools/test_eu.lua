-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- `user/lib/eu.lua` on the host (`docs/compute.md` C1): every line of
-- `tools/eu_corpus.txt` assembled and held bit for bit to the sixteen bytes
-- Mesa's `gentool` made of it for Skylake (`tools/eu_corpus.py`); then a
-- kernel with labels, a branch back and forward; and the lines it must
-- refuse, each with its number.

local eu = dofile("user/lib/eu.lua")
local checks, fails = 0, {}

local function check(ok, why)
  checks = checks + 1
  if not ok then fails[#fails + 1] = why end
end

local function hex(s)
  return (s:gsub(".", function(c) return ("%02x"):format(c:byte()) end))
end

-- The corpus, line by line.
local lines = 0

for row in io.lines("tools/eu_corpus.txt") do
  if not row:match("^#") and row:match("|") then
    local text, want = row:match("^(.-)%s*|%s*(%x+)%s*$")
    local got, why = eu.encode_line(text)

    lines = lines + 1
    check(got ~= nil and hex(got) == want,
          ("%s\n      wanted %s\n      got    %s"):format(text, want, got and hex(got) or ("nothing: " .. tostring(why))))
  end
end

check(lines >= 40, ("the corpus has %d lines, not the forty and more it was written with"):format(lines))

-- A kernel: a label behind and one ahead, the offsets from each branch.
local code, why = eu.assemble([[
// a loop that runs while any lane is still inside
loop:
mul (16|M0) r20.0<1>:f r16.0<8;8,1>:f r16.0<8;8,1>:f
cmp (16|M0) (lt)f0.0 null<1>:f r20.0<8;8,1>:f 0x40800000:f
(f0.0.any16h) jmpi (1) loop
jmpi (1) done
mov (8|M0) r112.0<1>:ud r0.0<8;8,1>:ud
done:
send.ts (16|M0) null r112 0x00000000 0x02000010 {EOT}
]])

check(code ~= nil and #code == 6 * 16, "the kernel did not assemble to six instructions: " .. tostring(why))

if code then
  local back = string.unpack("<i4", code, 2 * 16 + 13)
  local ahead = string.unpack("<i4", code, 3 * 16 + 13)

  check(back == -32, ("the branch back to loop was %d bytes, not -32"):format(back))
  check(ahead == 32, ("the branch to done was %d bytes, not 32"):format(ahead))
  check(code:sub(2 * 16 + 1, 2 * 16 + 1):byte() == 0x20, "the branch is not a jmpi")
  check(code:byte(6 * 16) & 0x80 == 0x80, "the last instruction does not end the thread")
end

-- What it refuses, and says where.
local function refused(text, line, words)
  local got, why = eu.assemble(text)

  check(got == nil and why and why:match("^line " .. line .. ":") and why:find(words, 1, true),
        ("%q was not refused at line %d for %q: %s"):format(text, line, words, tostring(why)))
end

refused("mov (16|M0) r10.0<1>:f r11.0<8;8,1>:f\nfrobnicate (16) r1 r2", 2, "frobnicate")
refused("mov (16|M0) r200.0<1>:f r11.0<8;8,1>:f", 1, "no register r200")
refused("jmpi (1) nowhere", 1, "no label nowhere")
refused("x:\nmov (8|M0) r1.0<1>:ud r2.0<8;8,1>:ud\nx:", 3, "the label x twice")
refused("send.ugm (16|M0) null r1 0x00000000 0x00000000", 1, "no unit ugm")
refused("add (7|M0) r10.0<1>:f r11.0<8;8,1>:f r12.0<8;8,1>:f", 1, "no execution size")
refused("", 0, "") -- replaced below

if #fails > 0 and fails[#fails]:match('^"" was not refused') then table.remove(fails) end

local empty, why_empty = eu.assemble("// nothing\n")
check(empty == nil and why_empty == "no instructions", "an empty kernel was not refused: " .. tostring(why_empty))

if #fails > 0 then
  print(("FAIL: %d of %d eu checks"):format(#fails, checks))
  for _, f in ipairs(fails) do print("  " .. f) end
  os.exit(1)
end

print(("PASS: %d eu checks (%d corpus lines held bit for bit to Mesa's assembler, a kernel's labels, "
       .. "and what is refused)"):format(checks, lines))
