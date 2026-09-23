-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- wc: how much is in a file.
--
--   wc notes.txt        lines, words and bytes
--
-- **Bytes, not characters.** The console decodes UTF-8 and one block
-- character is three bytes, so the two numbers differ the moment anything
-- outside ASCII is in the file. Bytes is what a filesystem stores and what
-- `ls` shows, so it is the one worth reporting without qualification.

local files = use("/lib/files.lua")
local text = use("/lib/text.lua")

local name = args:match("^%s*(%S+)")

if not name then
  print("wc: wc <path>")
  return
end

local path = files.abs(name, cwd)
local body, err = fs.read(path)

if body == nil then
  print("wc: " .. path .. ": " .. tostring(err))
  return
end

if type(body) ~= "string" then
  print("wc: " .. path .. " holds a " .. type(body) .. "; try `cat`")
  return
end

local words = 0

for _ in body:gmatch("%S+") do words = words + 1 end

print(("%d lines  %d words  %d bytes  %s")
      :format(#text.lines(body), words, #body, path))
