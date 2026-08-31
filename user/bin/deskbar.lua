-- kosmos: application
-- The Deskbar: what is running, and what can be.
--
--   wm                  starts this by itself
--   wm deskbar          the same, said out loud
--
-- Top right, because that is where BeOS put it and because it is the one
-- corner a window is least likely to want. It lists itself first - BeOS
-- called that entry Tracker - then every application that has registered,
-- in the order they started.
--
--------------------------------------------------------------------------
-- Where the two lists come from, and why neither is a list this program
-- keeps.
--
-- **What is running** is the window manager's own list of windows, not
-- `/app`. The registry holds applications that *registered*, which means
-- the ones that used `ui.window`; a program that opens a window by talking
-- to the desktop directly has a window on screen and no registration
-- anywhere, and the first two demonstrations here do exactly that because
-- they were written before there was a kit.
--
-- What belongs in a list of what is running is what is on the screen, and
-- the desktop is the only thing that knows that. It also means an
-- application that died leaves the list by itself, because its window went
-- with it.
--
-- **What can be run** is `/bin`, filtered by an attribute: a program says
-- `-- kosmos: application` on its first line and the program store reports
-- it as one. Guessing instead - looking for `ui.window` in the source -
-- would be the store deciding what a program is by reading it, and would be
-- wrong the first time somebody wrote the name in a comment.
--
-- Launching goes through the window manager rather than happening here,
-- because starting a windowed program means handing the new process an
-- endpoint to the desktop, and this program does not hold that endpoint. If
-- it did, everything that could reach this could reach the desktop's door.
--------------------------------------------------------------------------

local ui = use("/lib/ui.lua")
local theme = use("/lib/theme.lua")

local W, H = 210, 320

local screen = gfx.screen()
local sw = screen and select(1, screen:size()) or 1024

local win, err = ui.window{ title = "Tracker", w = W, h = H,
                            x = sw - W - 12, y = 34 }

if not win then
  print("deskbar: " .. tostring(err))
  return
end


--------------------------------------------------------------------------
-- What can be started.
--------------------------------------------------------------------------

local status = ui.label{ x = 10, y = H - 22, text = "", color = theme.text_dim }

local launchable = {}

do
  local names = fs.list("/bin") or {}

  for _, file in ipairs(names) do
    local attrs = fs.getattr("/bin/" .. file)

    if attrs and attrs.kind == "application" then
      local short = file:gsub("%.lua$", "")

      if short ~= "deskbar" then
        launchable[#launchable + 1] = short
      end
    end
  end

  table.sort(launchable)
end

win:add(ui.label{ x = 10, y = 8, text = "Running", color = theme.text })

local handles = {}

local running = ui.list{
  x = 10, y = 26, w = W - 20, h = 118, items = {},
  on_select = function(_, _, index)
    local handle = handles[index]

    if handle then
      fs.send("/dev/wm", { type = "raise", window = handle })
      status.text = "raised"
    end
  end,
}

win:add(running)

win:add(ui.label{ x = 10, y = 152, text = "Applications", color = theme.text })

local apps = ui.list{
  x = 10, y = 170, w = W - 20, h = 118, items = launchable,
  on_select = function(_, name)
    if not name then return end

    local ok, why = fs.send("/dev/wm", { type = "launch", program = name })
    status.text = ok and ("started " .. name)
                     or ("could not: " .. tostring(why))
  end,
}

win:add(apps)
win:add(status)

--------------------------------------------------------------------------
-- The running list, kept up to date.
--
-- A replicant-style tick rather than a poll of its own: the window kit
-- already wakes twice a second for anything that ticks, and asking /app
-- then costs one round trip at a rate a person can read.
--------------------------------------------------------------------------

local watcher = ui.view{ x = 0, y = 0, w = 0, h = 0 }

function watcher:tick()
  local reply = fs.send("/dev/wm", { type = "windows" })
  local names = {}

  handles = {}

  for i, w_ in ipairs(reply and reply.windows or {}) do
    names[i] = w_.title
    handles[i] = w_.handle
  end

  local same = #names == #running.items

  if same then
    for i = 1, #names do
      if names[i] ~= running.items[i] then
        same = false
        break
      end
    end
  end

  if not same then
    running.items = names

    if running.selected > #names then
      running.selected = #names > 0 and #names or 1
    end
  end
end

win:add(watcher)
win:run()
