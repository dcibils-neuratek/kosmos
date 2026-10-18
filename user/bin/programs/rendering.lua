-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- How the desktop is drawn, from the prompt (`docs/rendering.md`).
--
--   rendering        the modes, and what each does
--   rendering 2      the desktop switched to mode 2, Page flips - now, and
--                    until the window manager starts again; Preferences'
--                    Displays page is where a mode is kept
--   rendering 0 3    each in turn, a second apart
--
-- The window manager says which mode it is in, and what this machine gave
-- of the one asked for, in the log: `log wm`.

local rendering = use("/Kosmos/Libraries/rendering.lua")
local words = use("/Kosmos/Libraries/files.lua").words(args, 8)
local wants = {}

for _, w in ipairs(words) do
  local m = math.tointeger(tonumber(w))

  if m == nil or m < 0 or m > rendering.LAST then
    print(("rendering: rendering [0-%d ...]"):format(rendering.LAST))
    return
  end

  wants[#wants + 1] = m
end

-- Each asked for in turn - `rendering 0 3` tries Framebuffer and comes back
-- to Blitter copy - with a second between, for whoever is looking.
for i, want in ipairs(wants) do
  local r = fs.send("/Running/wm", { type = "rendering", mode = want })

  if type(r) == "table" and r.ok then
    print(("rendering: %d, %s%s"):format(r.mode, rendering.name(r.mode),
          r.mode ~= want and (" - the most this machine gives of " .. rendering.name(want)) or ""))
  else
    print("rendering: no window manager to ask")
    return
  end

  if i < #wants then sys.sleep((sys.info() or {}).tick_hz or 250) end
end

if #wants > 0 then return end

for m = 0, 5 do
  local mode = rendering.MODES[m]

  print(("%d  %-18s %s%s"):format(m, mode.name, mode.tag and ("(" .. mode.tag .. ") ") or "",
        mode.sentence))
end
