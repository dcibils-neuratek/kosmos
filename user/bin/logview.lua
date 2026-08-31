-- kosmos: application
-- The system log, in a window.
--
--   wm logview
--
-- Everything this machine has printed, kernel and processes together, in
-- the order it happened.
--
-- One place with all of it, and that is the interesting part. The kernel's
-- boot narration goes through `kputc`; so does every `print` from every
-- program, because a program prints by asking the console server and the
-- console server calls `sys.write`, which is `kputc`. So the kernel keeps a
-- ring of what went past and this reads it.
--
-- Before this, the only complete record was the serial line - which is on
-- the other end of a cable, and is exactly what you do not have when you
-- are looking at the screen.

local ui = use("/lib/ui.lua")
local theme = use("/lib/theme.lua")

local W, H = 620, 420

local win, err = ui.window{ title = "Log", w = W, h = H, x = 130, y = 110 }

if not win then
  print("logview: " .. tostring(err))
  return
end

local view = ui.text{ x = 10, y = 30, w = W - 20, h = H - 66, blocks = {} }

win:add(ui.label{ x = 10, y = 10, text = "what this machine has printed",
                  color = theme.text })
win:add(view)

local note = ui.label{ x = 10, y = H - 26, text = "", color = theme.text_dim }
win:add(note)

--------------------------------------------------------------------------
-- Re-read on a clock.
--
-- A tail rather than a subscription: the kernel keeps a ring and this reads
-- the end of it once a second. A subscription would mean the kernel calling
-- a process, which is the thing this system is arranged not to do - and a
-- log viewer that could block the kernel would be a poor trade for a
-- second of latency.
--------------------------------------------------------------------------

local last = ""

local ticker = ui.view{ x = 0, y = 0, w = 0, h = 0 }

function ticker:tick()
  local text = sys.log(6000)

  if not text or text == last then return end

  last = text

  local blocks = {}

  for line in (text .. "\n"):gmatch("([^\n]*)\n") do
    -- Carriage returns are for the serial line's benefit and are noise
    -- here; the screen has no idea what a carriage return is.
    line = line:gsub("\r", "")

    if line ~= "" then
      --
      -- A little colour, by shape rather than by any tagging: nothing that
      -- prints here declares a severity, so the only honest way to pick
      -- one out is by what it says.
      --
      local style = "body"

      if line:match("^PANIC") or line:match("died") or line:match("[Ee]rror")
         or line:match("could not") then
        style = "accent"
      elseif line:match("^%[%d+/%d+%]") then
        style = "head"
      end

      blocks[#blocks + 1] = { style = style, text = line }
    end
  end

  view.blocks = blocks

  -- Stuck to the bottom, which is where a log is read from. `content` is
  -- what the last draw measured, so this follows the text rather than
  -- guessing how tall it became.
  view.scroll = math.max(0, (view.content or 0) - view.h + 8)

  note.text = ("%d bytes, %d lines"):format(#text, #blocks)
end

win:add(ticker)
win:run()
