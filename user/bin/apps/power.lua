-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Generic
-- kosmos: name Shut Down
-- kosmos: section none
-- Restart or Shut Down, asked before it is done (`docs/launcher.html`,
-- agreed by Diego on 8 October).
--
--   power shutdown        what the launcher's Shut Down opens
--   power restart         and its Restart
--
-- In the middle of the screen: what is about to happen, Cancel, and the
-- action - Return takes it, Escape cancels - and it is done by itself after
-- 30 seconds unless cancelled, as KDE's is, so a machine somebody walked
-- away from still goes off. What it sends is what the Deskbar's menu sends,
-- `wm`'s `power`: the window manager holds the authority to end every
-- process, and this only asks.

local ui = use("/Kosmos/Libraries/ui.lua")
local theme = ui.theme

local which = (tostring(args or ""):match("^%s*(%S+)") == "restart") and "restart" or "shutdown"
local WORDS = {
  shutdown = { title = "Shut Down", ask = "Shut down Kosmos?", doing = "Shutting down",
               action = "off", icon = "power" },
  restart  = { title = "Restart", ask = "Restart Kosmos?", doing = "Restarting",
               action = "restart", icon = "reload" },
}
local w = WORDS[which]

local WAIT = 30                  -- seconds before it acts by itself
local W, H = 460, 168

local win, err = ui.window{ title = w.title, w = W, h = H, centre = true }

if not win then
  print("power: " .. tostring(err))
  return
end

local counter_hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000
local since = sys.ticks()
local left = WAIT
local done = false

local function act()
  if done then return end

  done = true
  print("power: " .. w.action)
  fs.send("/Running/wm", { type = "power", action = w.action })
end

-- The picture, the question, what happens to applications, and the count.
local body = ui.view{ x = 0, y = 0, w = W, h = H - 64 }

function body:draw(g)
  local face = theme.window

  g:fill(0, 0, self.w, self.h, face)
  g:fill_round(24, 22, 56, 56, theme.raised, 28)
  g:line_icon(24 + (56 - 30) // 2, 22 + (56 - 30) // 2, w.icon, theme.accent, 30)
  g:text(96, 24, w.ask, theme.text, face, "heading")
  g:text(96, 24 + gfx.height("heading") + 6,
         "Every application is asked to close first.", theme.text_dim, face)
  g:text(96, 24 + gfx.height("heading") + 6 + gfx.height() + 10,
         ("%s by itself in %d s"):format(w.doing, left), theme.text, face, "mono")
end

local cancel = ui.button{ text = "Cancel" }
local go = ui.button{ text = w.title, go = true }

go.x = W - 24 - go.w
go.y = H - 64 + 14
cancel.x = go.x - 10 - cancel.w
cancel.y = go.y

cancel.on_click = function() win:close() end
go.on_click = act

win:add(body)
win:add(cancel)
win:add(go)
win:focus_on(go)

function win:on_key(c)
  if c == 27 then
    self:close()
    return true
  elseif c == 10 or c == 13 then
    act()
    return true
  end

  return false
end

-- The count, a second at a time; at none, done.
function win:on_frame()
  self.poll_wait_ticks = 10

  local now_left = math.max(0, WAIT - (sys.ticks() - since) // counter_hz)

  if now_left ~= left then
    left = now_left
    self.dirty = true

    if left == 0 then act() end

    return true
  end

  return false
end

print(("power: asking to %s, %d s"):format(which, WAIT))
win:run()
