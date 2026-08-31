-- kosmos: application
-- The look of the desktop: which palette, and what colour the ground is.
--
--   wm appearance
--
-- Haiku keeps this under Preferences and calls it Appearance, which is
-- where the name comes from.
--
-- Everything here is one message to the window manager. It holds the
-- palette because it is the one process already talking to every window;
-- it repaints the desktop and forwards the change, and each application's
-- widget kit updates its own colours without the application knowing. So
-- this program has no drawing code of its own beyond its own window, and
-- changing a colour here changes every window on screen.
--
-- The choice is written to `/home/.appearance` and read back at startup, so
-- it survives the power going off. That file is the first thing in this
-- system that is a *setting* rather than data, and it is stored as an
-- ordinary Lua table through the ordinary filesystem protocol.

local ui = use("/lib/ui.lua")

local W, H = 380, 320

local win, err = ui.window{ title = "Appearance", w = W, h = H,
                            x = 200, y = 140 }

if not win then
  print("appearance: " .. tostring(err))
  return
end

local SETTINGS = "/home/.appearance"

-- Desktop colours to choose from.
--
-- A fixed set rather than a full picker: three sliders and a hue wheel is a
-- lot of widget kit for a decision made once, and a row of swatches is how
-- this was done in 1998 anyway. Two rows - greys and blues that sit under a
-- dark palette, then lighter grounds for the light one.
local SWATCHES = {
  0xff1c2530, 0xff11161d, 0xff232b36, 0xff2b2b2b,
  0xff1b2a3a, 0xff223344, 0xff2a3f2a, 0xff3a2a3a,
  0xff336699, 0xff4a7ab0, 0xff6688aa, 0xff8899aa,
  0xff707070, 0xff909090, 0xffb0b0b0, 0xffd0d0d0,
}

local SW      = 40             -- a swatch, in pixels
local PER_ROW = 8

local chosen_palette = "dark"
local chosen_desktop = nil     -- nil means "whatever the palette says"

local status = ui.label{ x = 12, y = H - 60, w = W - 24, text = "" }

local function send()
  -- Both return values. `fs.send` answers `nil, reason` when the server
  -- said no, so a caller that looks only at the first one reports "no
  -- reply" for every refusal and throws away what was actually wrong.
  local reply, why = fs.send("/dev/wm", { type = "theme",
                                          palette = chosen_palette,
                                          desktop = chosen_desktop })

  if not reply then
    status.text = "refused: " .. tostring(why)
    return
  end

  -- Written only after the window manager accepted it, so the file cannot
  -- come to hold an appearance the system never managed to apply.
  local ok, werr = fs.write(SETTINGS, { palette = chosen_palette,
                                        desktop = chosen_desktop })

  status.text = ok and ("saved: " .. chosen_palette)
                or ("applied, but not saved: " .. tostring(werr))
end

win:add(ui.label{ x = 12, y = 10, w = W - 24, text = "Palette" })

win:add(ui.button{
  x = 12, y = 32, w = 110, h = 26, text = "Dark",
  on_click = function() chosen_palette = "dark"; send() end,
})

win:add(ui.button{
  x = 132, y = 32, w = 110, h = 26, text = "Light",
  on_click = function() chosen_palette = "light"; send() end,
})

win:add(ui.label{ x = 12, y = 76, w = W - 24, text = "Desktop" })

-- The swatches, as a view that draws itself and answers a click.
--
-- Not sixteen buttons: a button is a bevel and a label and a focus ring,
-- and what this wants is a colour and nothing else. `gfx.md`'s rule holds
-- either way - the fills are C, and what Lua decides is where they go.
local swatches = ui.view{
  x = 12, y = 98, w = PER_ROW * SW, h = 2 * SW,

  draw = function(self, g)
    for i, colour in ipairs(SWATCHES) do
      local col = (i - 1) % PER_ROW
      local row = (i - 1) // PER_ROW
      local x, y = col * SW, row * SW

      g:fill(x, y, SW - 2, SW - 2, colour)

      if colour == chosen_desktop then
        g:frame(x, y, SW - 2, SW - 2, ui.theme.ring)
        g:frame(x + 1, y + 1, SW - 4, SW - 4, ui.theme.ring)
      else
        g:frame(x, y, SW - 2, SW - 2, ui.theme.line)
      end
    end
  end,

  on_click = function(self, x, y)
    local col = x // SW
    local row = y // SW
    local i = row * PER_ROW + col + 1

    if SWATCHES[i] then
      chosen_desktop = SWATCHES[i]
      send()
    end
  end,
}

win:add(swatches)

win:add(ui.button{
  x = 12, y = 98 + 2 * SW + 12, w = 160, h = 26, text = "Palette default",
  on_click = function() chosen_desktop = nil; send() end,
})

win:add(status)

-- What is in force now, so the window opens saying the truth rather than a
-- guess. The file is the record of what was chosen; a machine with no disk
-- simply has not chosen anything.
local saved = fs.read(SETTINGS)

if type(saved) == "table" then
  chosen_palette = saved.palette or chosen_palette
  chosen_desktop = saved.desktop
  status.text = "in force: " .. tostring(chosen_palette)
else
  status.text = "in force: dark (nothing saved yet)"
end

win:run()
