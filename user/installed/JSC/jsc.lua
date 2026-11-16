-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- jsc: run JavaScript, with WebKit's JavaScriptCore.
-- kosmos: image jsc.elf
--
--   jsc script.js           the script in that file
--   jsc -e "print(6 * 7)"   the script given
--   jsc --no-jit ...        the same, on the interpreter alone
--
-- **An installed program** (`docs/w4.md` W4d): this file and `jsc.elf`,
-- the engine - JavaScriptCore with its interpreter, WTF, bmalloc, ICU and
-- its data - in one folder, `/Home/Apps/JSC`. The system's image carries
-- none of it. `print` is the one function the script is given beyond the
-- language.

local files = use("/Kosmos/Libraries/files.lua")
local have, jsc = pcall(use, "jsc.elf")

if not have or type(jsc) ~= "table" then
  print("jsc: " .. tostring(jsc))
  return
end

local words = files.words(args)
local said, ok

local interpreted = words[1] == "--no-jit"
if interpreted then
  table.remove(words, 1)
  if not jsc.no_jit() then
    print("jsc: the JIT could not be turned off")
    return
  end
end

if words[1] == "-e" and words[2] then
  said, ok = jsc.eval(words[2])
elseif words[1] then
  said, ok = jsc.run(files.abs(words[1], cwd))
else
  print("usage: jsc [--no-jit] <script.js>  or  jsc [--no-jit] -e \"<script>\"")
  return
end

-- Line by line, as `print` gives them: a program has no stream to write to.
for line in said:gmatch("([^\n]*)\n") do
  print(line)
end
-- What says the switch held: the JIT's writer wrote nothing.
if interpreted then
  local _, bytes = jsc.jit_pages()
  print(("jsc: on the interpreter alone, %d bytes of code written"):format(bytes))
end
return ok
