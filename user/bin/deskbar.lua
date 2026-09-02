-- kosmos: application
-- kosmos: needs screen
-- The Deskbar: what is running, and what can be.
--
--   wm                  starts this by itself
--   wm deskbar          the same, said out loud
--
-- Top right, because that is where BeOS put it and because it is the one
-- corner a window is least likely to want. It lists itself first, then every
-- application that has registered, in the order they started.
--
-- **This window used to be titled "Tracker", and that was wrong.** In BeOS
-- the two are different programs doing different jobs: the Deskbar is this -
-- what is running and what can be started - and Tracker is the file
-- manager. Tracker appears in the Deskbar's list because it is always
-- running, which is presumably how the name ended up on this window. There
-- is no Tracker in Kosmos yet; when there is, it will be a file manager and
-- it will appear in the list below like anything else.
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
-- The *kit's* palette, not a copy of it.
--
-- `use` runs the chunk again and hands back a different table, and only the
-- one `ui.lua` holds is the one it mutates when the desktop changes theme.
-- An application that loaded its own kept the colours it started with while
-- every widget around it changed - which is exactly what Monitor, Processes,
-- Photo and the Terminal did.
local theme = ui.theme

local screen = gfx.screen()
local sw, sh = 1024, 768

if screen then sw, sh = screen:size() end


--------------------------------------------------------------------------
-- What can be started.
--------------------------------------------------------------------------

local launchable = {}

--
-- Three sections, the way BeOS's Be menu had three folders.
--
-- It sorted by *directory* - /boot/apps, /boot/demos, /boot/preferences -
-- and the menu was those folders. Kosmos has one `/bin`, so each file says
-- which section it is in and the program store reports it; see
-- `kosmos: section` in the header of any application.
--
local SECTIONS = { "applications", "demos", "preferences" }
local by_section = { applications = {}, demos = {}, preferences = {} }

do
  for _, file in ipairs(fs.list("/bin") or {}) do
    local attrs = fs.getattr("/bin/" .. file)

    if attrs and attrs.kind == "application" then
      local short = file:gsub("%.lua$", "")

      -- The Deskbar does not list itself. It is not something you start.
      if short ~= "deskbar" then
        local into = by_section[attrs.section or "applications"]
                     or by_section.applications

        into[#into + 1] = short
        launchable[#launchable + 1] = short
      end
    end
  end

  for _, name in pairs(by_section) do table.sort(name) end
  table.sort(launchable)
end

--------------------------------------------------------------------------
-- Sized to what there is, not to what there was.
--
-- This was two fixed lists of seven rows, chosen when there were five
-- applications. There are more now and there will be more again, and a
-- launcher that cannot show you everything it can launch is a launcher with
-- a bug in it. So the applications list is as tall as the list of
-- applications, and the window is as tall as that needs - clamped to the
-- screen, after which the list scrolls, which it already knew how to do.
--------------------------------------------------------------------------

local ROW = gfx.font.h
local W = 210

--
-- A button and a list, and that is the whole window now.
--
-- It used to carry every application in a list of its own, which at
-- twenty-three of them was most of the height of the screen and still
-- needed scrolling. BeOS put them in a *menu* instead - three folders,
-- opened from one button - and that is right for the same reason it is
-- right anywhere: a launcher you use once a minute should not be occupying
-- the screen the rest of the time.
--
local BUTTON_H = ROW + 12
local RUNNING_ROWS = 10

local running_h = RUNNING_ROWS * ROW + 6
local H = 12 + BUTTON_H + 12 + 18 + running_h + 30

local win, err = ui.window{ title = "Deskbar", w = W, h = H,
                            x = sw - W - 12, y = 34 }

if not win then
  print("deskbar: " .. tostring(err))
  return
end

local status = ui.label{ x = 10, y = H - 22, text = "",
                         color = "text_dim" }

win:add(ui.label{ x = 10, y = 12 + BUTTON_H + 12, text = "Running",
                  color = "text" })

local handles = {}

local running = ui.list{
  x = 10, y = 12 + BUTTON_H + 12 + 18, w = W - 20, h = running_h, items = {},
  on_select = function(_, _, index)
    local handle = handles[index]

    if handle then
      fs.send("/dev/wm", { type = "raise", window = handle })
      status.text = "raised"
    end
  end,
}

win:add(running)



--
-- One button, and the menu comes out of it.
--
-- Opened downward from the button's bottom-left corner, which is where a
-- menu belongs relative to the thing that opened it. `win.origin_x` is
-- where this window's content starts on the screen; a menu is a window of
-- its own and is placed on the screen, not inside this one.
--
local function section_items(which)
  local out = {}

  for _, name in ipairs(by_section[which] or {}) do
    out[#out + 1] = {
      text = name,
      on_choose = function()
        local ok, why = fs.send("/dev/wm", { type = "launch", program = name })

        status.text = ok and ("started " .. name)
                         or ("could not: " .. tostring(why))
      end,
    }
  end

  if #out == 0 then out[1] = { text = "(nothing here)" } end

  return out
end

local menu_button
menu_button = ui.button{
  x = 10, y = 12, w = W - 20, h = BUTTON_H, text = "Kosmos",
  on_click = function()
    local items = {}

    for _, which in ipairs(SECTIONS) do
      items[#items + 1] = {
        -- Capitalised for the menu, lower case everywhere else, because a
        -- section is a name here and an identifier in the header.
        text = which:sub(1, 1):upper() .. which:sub(2),
        submenu = section_items(which),
      }
    end

    win:open_menu(win.origin_x + menu_button.x,
                  win.origin_y + menu_button.y + menu_button.h,
                  items)
  end,
}

win:add(menu_button)
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

  --
  -- Sorted by handle, which is the order the windows were opened in and
  -- never changes.
  --
  -- The window manager returns them in *stacking* order, and stacking order
  -- moves every time anything is raised - including by the very click that
  -- is about to land on this list. So the row under the pointer changed
  -- between the press and the release, and you restored whatever had taken
  -- the place of the thing you aimed at.
  --
  -- `procs` hit this first and its comment says it best: "you aim at one and
  -- end another". Its answer was to follow the selection by identity; this
  -- one's is simpler because a window's handle never changes - put them in
  -- an order that has nothing to do with what is on top.
  --
  local list = {}

  for _, w_ in ipairs(reply and reply.windows or {}) do
    list[#list + 1] = w_
  end

  --
  -- Newest first, which is the opposite of the order they opened in.
  --
  -- Sorted rather than reversed-as-found, because the window manager hands
  -- them back in stacking order and stacking order moves when anything is
  -- raised - including by the click about to land on this list. A handle
  -- never changes, so this order does not move under the pointer. `procs`
  -- learned the same thing: "you aim at one and end another".
  --
  table.sort(list, function(a, b) return a.handle > b.handle end)

  for i, w_ in ipairs(list) do
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
