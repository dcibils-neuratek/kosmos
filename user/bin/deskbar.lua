-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Deskbar
-- kosmos: needs screen network audio
-- The Deskbar: the strip across the top, and everything you reach from it.
--
--   wm                  starts this by itself
--   wm deskbar          the same, said out loud
--
-- The Kosmos menu at the left, a button per running window across the
-- middle, and what the machine is doing at the right. **One bar**: there
-- used to be this in the top-right corner and a separate `topbar` with five
-- hard-coded shortcuts, which was a menu that could not be edited sitting
-- next to a menu that could.
--
-- BeOS put its Deskbar in the corner and that is the one thing here taken
-- from Windows instead, deliberately: a corner tile is charming, and a
-- person hunting for a window wants one place that is always the same
-- shape.
--
-- **`network` and `audio` are for the indicators, and they are a change of
-- position worth naming.** This used to declare `needs screen` and nothing
-- else, and the paragraph below said why - launching goes through the
-- window manager so that reaching the Deskbar is not reaching everything.
-- That is still true of *power*: Restart and Shut Down are a request this
-- sends, because `processes` stays with the window manager. What changed is
-- that the bar is where a person now manages the machine from, and a bar
-- that cannot see the volume cannot show it. Reading state is not holding
-- power, and the two are kept apart on purpose.
--
-- Neither widens what this can reach on a machine that lacks the hardware:
-- `init.lua` grants `audio` only when there is a sound card, so on a board
-- without one `/dev/audio` is not in this namespace and the speaker is not
-- drawn. The old rule - never draw an indicator for a subsystem that does
-- not exist - is now enforced by the kernel rather than remembered by a
-- comment.
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
-- **What can be run** is `/home/Deskbar`: a folder per section, holding
-- launcher files. A thing is in the menu because somebody made a launcher
-- for it, and moving, renaming or giving one arguments is moving, renaming
-- or setting an attribute on a file - Tracker's job, not this program's.
--
-- It was `/bin` filtered by an attribute, with each program's header saying
-- which section it belonged to. That made the menu read-only: `/bin` is an
-- array the build puts in the binary, so moving an item meant editing
-- source and rebuilding, and an item could carry no arguments at all. The
-- header's `kosmos: section` is now only the default a program is filed
-- under the first time the tree is made.
--
-- Launching goes through the window manager rather than happening here,
-- because starting a windowed program means handing the new process an
-- endpoint to the desktop, and this program does not hold that endpoint.
-- The same argument keeps `processes` there: the authority to end every
-- process at once lives in one place, and what this menu sends is a request
-- rather than a right.
--------------------------------------------------------------------------

local ui = use("/lib/ui.lua")
local menudata = use("/lib/deskbarmenu.lua")
local clock = use("/lib/clock.lua")
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
-- What can be started, and where the menu comes from.
--
-- **The Deskbar is a menu of launchers, and a thing appears in it because a
-- launcher file exists.** `/home/Deskbar` holds a folder per section, each
-- holding launchers; a folder inside one of those is a submenu. Moving an
-- item between sections is moving the file, renaming it is renaming the
-- file, and giving it arguments - Doom at a different size - is an
-- attribute on it. All of that is Tracker's job already, and none of it is
-- this program's.
--
-- **That is BeOS's answer, and this is the second time round to it.** The
-- Be menu sorted by *directory* - /boot/apps, /boot/demos,
-- /boot/preferences - and you moved a program between menus by moving the
-- file. Kosmos collapsed that to one `/bin` with the section written into
-- each program's own header, which read as a simplification and was a
-- mistake: `/bin` is an array the build puts in the binary, so a menu built
-- from it is read-only by construction. Moving 3dcube out of Demos meant
-- editing its source and rebuilding the image, and a menu item could carry
-- no arguments at all, because a header line is a fact about the *program*
-- and arguments are a choice about a *use* of it.
--
-- `kosmos: section` is now the *default* a program is filed under the first
-- time it is seen, and nothing more. See `seed` below.
--
-- **No launcher, no entry.** There is deliberately no fallback that lists a
-- program with no launcher, and no ledger of programs seen before: both
-- were considered and both exist only to patch a fallback. A program built
-- later is invisible until somebody makes a launcher for it, which is the
-- rule working rather than a gap in it - `/bin` is in the namespace, so
-- Tracker can open it and a launcher can be made from there.
--------------------------------------------------------------------------

local DESKBAR = "/home/Deskbar"

--
-- What `/bin` can start, which is a different question from what the menu
-- lists and is still worth asking.
--
-- Startup items name a *program*, and are checked against this rather than
-- against the menu: "open this at login" and "show this in the menu" are
-- two choices, and an item can reasonably be one without the other. The
-- seed reads it too, for the section and icon each program declares.
--
local programs = {}
local launchable = {}

do
  for _, file in ipairs(fs.list("/bin") or {}) do
    local attrs = fs.getattr("/bin/" .. file)

    if attrs and attrs.kind == "application" then
      local short = file:gsub("%.lua$", "")

      -- The Deskbar does not list itself. It is not something you start.
      if short ~= "deskbar" then
        programs[short] = attrs
        launchable[#launchable + 1] = short
      end
    end
  end

  table.sort(launchable)
end

--
-- The tree, made once, on a machine that has never had one.
--
-- A first run rather than a fallback, and the difference is the whole rule
-- above: this makes a menu where there is *no* menu, and never adds an item
-- to one that exists. So a launcher deliberately thrown away stays thrown
-- away, which a per-item fallback could not promise.
--
-- Tracker does the same for `/home/Desktop`, Drive, the Trash and the cheat
-- sheet, and for the same reason it gives: a folder that only exists once
-- you think to make one is a folder nobody makes.
--
-- `/home` is always there to put it in - on the disk when there is one, and
-- moved into memory by `init.lua` when there is not - so a machine with no
-- drive gets a menu too, and loses it at the power switch along with
-- everything else it wrote.
--
local function seed()
  if fs.getattr(DESKBAR) then return end

  local ok, why = fs.send(DESKBAR, { type = "mkdir" })

  if not ok then
    print("deskbar: no " .. DESKBAR .. ": " .. tostring(why))
    return
  end

  local made = {}

  for _, short in ipairs(launchable) do
    local attrs = programs[short]
    local said = attrs.section or "applications"
    local where, group = said:match("^([^/]+)/(.+)$")

    where = where or said

    -- Capitalised here and nowhere else. The folder's name *is* what the
    -- menu shows from now on, so this is the one moment the identifier in a
    -- program's header becomes a word on the screen.
    local folder = DESKBAR .. "/" .. where:sub(1, 1):upper() .. where:sub(2)

    if group then folder = folder .. "/" .. group end

    if not made[folder] then
      made[folder] = true

      if not fs.getattr(folder) then
        local fine, oops = fs.send(folder, { type = "mkdir" })

        if not fine then
          print(("deskbar: no %s: %s"):format(folder, tostring(oops)))
        end
      end
    end

    local path = folder .. "/" .. short
    local fine, oops = fs.write(path, "")

    if fine then
      --
      -- The whole path, not the short name.
      --
      -- `handlers.launch` accepts either - a bare name becomes
      -- `/bin/<name>.lua` - and what a *file* records should not depend on
      -- a completion rule the file cannot state. A launcher that says
      -- `/bin/doom.lua` says what it runs; one that says `doom` says what
      -- it runs only to somebody who knows the rule, and reads as broken to
      -- anybody who does not.
      --
      -- Launchers already written the short way keep working, because the
      -- window manager still completes a bare name. Nothing has to be
      -- migrated.
      --
      fine, oops = fs.setattr(path, { kind = "launcher", type = "launcher",
                                      program = "/bin/" .. short .. ".lua",
                                      args = "", icon = attrs.icon })
    end

    if not fine then
      print(("deskbar: no launcher for %s: %s"):format(short, tostring(oops)))
    end
  end

  print(("deskbar: made %s from what /bin declares"):format(DESKBAR))
end

--
-- And read back, once.
--
-- `deskbarmenu.lua` is the part with the decisions in it - what counts as
-- an item, what order things come in - and is tested on the build machine
-- without a boot; this is the part that draws.
--
-- **Once at startup, and again when something says so.** Reading the tree
-- is about fifty messages to the disk server - a listing per folder and a
-- `getattr` per file - and doing that on every click to catch a change
-- somebody made deliberately is paying for ever to avoid asking. The menu
-- is opened far more often than it is edited.
--
-- Asking is `menu`, which is published below and written the way anything
-- running is written to:
--
--   setprop /app/Deskbar/menu reload
--
-- No new mechanism and no new program: `ui.window` registers this window
-- with `/app` and answers for its properties, and `setprop` is the general
-- four-line program that writes one. The editor sends the same thing after
-- it changes a launcher, so the menu is right without anybody being told to
-- do anything.
--
local sections = {}

local function read_sections()
  sections = menudata.sections(fs, DESKBAR)
end

seed()
read_sections()

--------------------------------------------------------------------------
-- The strip across the top, which is the whole of the Deskbar now.
--
-- **One bar, not two.** There was a Deskbar in the top-right corner - a
-- button and a list of what was running - and a separate top bar with five
-- shortcuts and a clock. Two pieces of always-present chrome, one of which
-- duplicated the other's job badly: the shortcuts were a fixed list of five
-- names in `topbar.lua`, which is a menu that cannot be edited, next to a
-- Deskbar whose menu could.
--
-- So: the Kosmos menu at the left, a button per running window across the
-- middle, and what the machine is doing at the right. That is the Windows
-- taskbar's arrangement rather than BeOS's corner, chosen deliberately -
-- a corner tile is charming and a person hunting for a window wants one
-- place that is always the same shape.
--
-- **36 pixels, with 32-pixel icons.** An icon large enough to recognise is
-- what makes a row of buttons scannable rather than a row of words, and 32
-- is the size every icon in `assets/icons/` actually is - nothing here
-- scales one, so any other number would be a crop.
--
-- Drawn as one view rather than as widgets, which is `topbar.lua`'s
-- decision and its reasoning holds: a button is a bevel, a label and a
-- focus ring, and none of those belong on a bar. What this wants is
-- something you can click, which is a fill and a picture.
--------------------------------------------------------------------------


--------------------------------------------------------------------------
-- What the machine is doing, asked carefully.
--
-- **Both of these may be unanswerable, and that is not an error.** A
-- capability this process was not granted means the device is not in its
-- namespace at all - `/dev/audio` is not denied, it is absent - so the
-- honest answer is "no indicator" rather than a picture of silence or a
-- crossed-out aerial. `topbar.lua` refused to draw indicators for
-- subsystems that did not exist and was right; the difference now is that
-- the kernel decides, not a comment.
--
-- Wrapped in `pcall` because a library that reaches a server it cannot see
-- raises rather than returning nil, and a bar that stops drawing because
-- the sound card is missing would be a worse failure than no speaker.
--------------------------------------------------------------------------

local audio_lib = nil

do
  local ok, got = pcall(use, "/lib/audio.lua")

  audio_lib = ok and got or nil
end

--
-- The master gain, 0 to 256, or nil when there is no audio to ask about.
--
local function volume_now()
  if not audio_lib then return nil end

  local ok, stats = pcall(audio_lib.stats)

  if not ok or not stats or not stats.master then return nil end

  return stats.master
end

--
-- Whether there is a card, which is the one thing about the network a bar
-- can say in an icon. `network.lua` is where the rest of it lives, and
-- clicking the icon opens it.
--
-- `fs.net_info("/net")` is what that program asks, and it is the only thing
-- that answers: there is no `/dev/net`. The devices server serves `cpu`,
-- `clock`, `screen`, `keyboard`, `memory` and `cores`, and the network is
-- not one of them - it is reached through the capability rather than
-- through a device file, which is why `needs network` is the whole of the
-- access check.
--
--
-- How busy the machine is, as a percentage, and how much memory is in use.
--
-- The same two readings `monitor.lua` draws, from the same two device
-- files: `/dev/kernel` counts idle and busy ticks and `/dev/memory` says
-- how much there is. Busy is a *difference* between two readings, so the
-- first pass has nothing to say and reports nothing rather than nought -
-- which would be a bar claiming an idle machine before it had looked.
--
-- No battery here, and it is the one indicator that would be a lie: this
-- machine cannot read one. It arrives when the ThinkPad's embedded
-- controller does, which is its own piece of work.
--
local last_idle, last_busy = nil, nil

local function load_now()
  local ok, k = pcall(fs.read, "/dev/kernel")

  if not ok or type(k) ~= "table" then return nil end

  local pct = nil

  if last_idle then
    local di = (k.idle_ticks or 0) - last_idle
    local db = (k.busy_ticks or 0) - last_busy

    if di + db > 0 then pct = (db * 100) // (di + db) end
  end

  last_idle, last_busy = k.idle_ticks, k.busy_ticks

  return pct
end

local function memory_now()
  local ok, m = pcall(fs.read, "/dev/memory")

  if not ok or type(m) ~= "table" or not m.total_mb then return nil end

  return m.total_mb - m.free_mb, m.total_mb
end

local function network_now()
  local ok, info = pcall(fs.net_info, "/net")

  if not ok or type(info) ~= "table" then return nil end

  return info
end


local H = 36
local ICON = 32
local W = sw

local win, err = ui.window{ title = "Deskbar", w = W, h = H, strip = "top" }

if not win then
  print("deskbar: " .. tostring(err))
  return
end

--
-- `setprop /app/Deskbar/menu reload` - the menu, read again.
--
-- Reading it costs about fifty messages to the disk server, so it happens
-- when somebody says the tree changed rather than on every click. Anything
-- that edits a launcher sends this afterwards; a person who moved one in
-- Tracker uses Reload Menus on the Kosmos button, and reading it back says
-- how many items are there now, which is how you tell a menu that did not
-- notice from a launcher that was not made. `Deskbar` with the capital is
-- the window's title, which is the name `ui.window` registers it under.
--
win:publish("menu",
  function()
    local n = 0

    for _, section in ipairs(sections) do n = n + #section.items end

    return ("%d sections, %d items"):format(#sections, n)
  end,
  function()
    read_sections()
    win.dirty = true
  end)

--------------------------------------------------------------------------
-- What is running, asked twice a second.
--
-- The window manager's list of windows, not `/app`: the registry holds
-- applications that *registered*, and a program that opens a window by
-- talking to the desktop directly has a window on screen and no
-- registration anywhere. What belongs on a taskbar is what is on the
-- screen, and the desktop is the only thing that knows that.
--
-- Sorted by handle, which is the order the windows opened in and never
-- changes. The reply comes back in *stacking* order, and stacking order
-- moves every time anything is raised - including by the click about to
-- land on this bar. `procs` learned the same thing the same way: "you aim
-- at one and end another".
--------------------------------------------------------------------------

local running = {}          -- { handle, title, icon, focused, hidden }

--
-- What picture a window gets, by what was started to make it.
--
-- The window manager reports the *program* - a path - and `/bin` reports
-- what each program's header declares. Asked once per program and
-- remembered, because a header cannot change while the machine is running
-- and asking on every pass would be a message per window per half second
-- for an answer that never moves.
--
local icon_of = {}

local function picture(program)
  if not program then return "App_Generic" end

  if icon_of[program] == nil then
    local attrs = fs.getattr(program)

    icon_of[program] = (attrs and attrs.icon) or "App_Generic"
  end

  return icon_of[program]
end

local watcher = ui.view{ x = 0, y = 0, w = 0, h = 0 }

function watcher:tick()
  local reply = fs.send("/app/wm", { type = "windows" })
  local list = {}

  --
  -- Chrome is not something that is running. The desktop and this bar are
  -- windows, because everything here is, but neither is an application you
  -- started or one you can switch to. The window manager marks them.
  --
  for _, w_ in ipairs(reply and reply.windows or {}) do
    if not w_.chrome then list[#list + 1] = w_ end
  end

  table.sort(list, function(a, b) return a.handle < b.handle end)

  local changed = (#list ~= #running)

  for i, w_ in ipairs(list) do
    local was = running[i]

    if not was or was.handle ~= w_.handle or was.title ~= w_.title
       or was.focused ~= w_.focused or was.hidden ~= w_.hidden then
      changed = true
    end

    running[i] = { handle = w_.handle, title = w_.title,
                   icon = picture(w_.program),
                   focused = w_.focused, hidden = w_.hidden }
  end

  for i = #running, #list + 1, -1 do running[i] = nil end

  if changed then win.dirty = true end
end

win:add(watcher)


--
-- What the Deskbar has to say, which is now the log.
--
-- There was a status label along the bottom of the old window. A bar across
-- the top of the screen has nowhere to put a line of text, and finding it a
-- corner would be a status bar on a status bar. `log deskbar` at the prompt
-- reads these, and `logview` is one of the things this menu starts.
--
local function say(text)
  print("deskbar: " .. tostring(text))
end

--
-- Right-click a row: edit the launcher behind it.
--
-- The kit closes the menu and says which item; `item.path` is the file,
-- which `launcher` below puts there precisely so this can find it. A
-- section or a submenu has no file to edit and says so rather than opening
-- a window about nothing.
--
-- Started through the window manager like everything else here, so it is
-- handed the desktop's endpoint and this program still holds no authority
-- it did not have. `launcheredit` writes the attributes and tells this
-- Deskbar to read its tree again, which is why nothing below has to.
--
function win:on_menu_context(item)
  if not item.path or item.folder then
    say((item.text or "that") .. " is not a launcher")
    return
  end

  local ok, why = fs.send("/app/wm", { type = "launch",
                                       program = "launcheredit",
                                       args = item.path })

  say(ok and ("editing " .. item.text)
      or ("could not open it: " .. tostring(why)))
end



--
-- One button, and the menu comes out of it.
--
-- Opened downward from the button's bottom-left corner, which is where a
-- menu belongs relative to the thing that opened it. `win.origin_x` is
-- where this window's content starts on the screen; a menu is a window of
-- its own and is placed on the screen, not inside this one.
--
--
-- One launcher, as a row.
--
-- The file's name is what the row says, so renaming the launcher renames
-- the item - which is the whole point of the menu being files. The program
-- it starts and the arguments it starts with are attributes, and both go in
-- the `launch` message: this is the same message Tracker sends when a
-- launcher on the desktop is opened, and the same one the menu has always
-- sent. What is new is only that there is something to put in `args`.
--
local function launcher(item)
  return {
    text = item.name,
    icon = item.icon or "App_Generic",

    -- The file this row came from, so a right press on it has something to
    -- edit. It is the one piece of the tree that has to survive into the
    -- menu: everything else a row needs is already on it.
    path = item.path,
    on_choose = function()
      if item.program == "" then
        say(item.name .. ": a launcher that names no program")
        return
      end

      local ok, why = fs.send("/app/wm", { type = "launch",
                                           program = item.program,
                                           args = item.args })

      say(ok and ("started " .. item.program)
          or ("could not: " .. tostring(why)))
    end,
  }
end

--
-- A folder's rows, and a folder inside it is a submenu.
--
-- Ordering is `read_folder`'s: submenus first, then launchers, each sorted
-- by name. Nothing is sorted here, because the order a person sees should
-- be the order the tree has.
--
local function folder_items(items)
  local out = {}

  for _, item in ipairs(items) do
    if item.folder then
      out[#out + 1] = { text = item.name, icon = "Folder_generic",
                        folder = true, path = item.path,
                        submenu = folder_items(item.items) }
    else
      out[#out + 1] = launcher(item)
    end
  end

  if #out == 0 then out[1] = { text = "(nothing here)" } end

  return out
end

--
-- The Kosmos menu, opened from the left end of the bar.
--
-- A function rather than a button's handler, because the bar is one view
-- and not a row of widgets - see the strip below. `bar:mouse` calls this
-- when a press lands left of `KOSMOS_W`.
--
local function open_kosmos_menu()
    local items = {}

    for _, section in ipairs(sections) do
      items[#items + 1] = {
        -- The folder's own name, which is what the menu shows. There is no
        -- capitalising left to do here: `seed` did it once, when a
        -- program's lower-case `kosmos: section` became a folder, and from
        -- then on the name on the screen is the name on the disk.
        text = section.name,
        icon = "Folder_generic",
        submenu = folder_items(section.items),
      }
    end

    if #items == 0 then
      items[1] = { text = "(no " .. DESKBAR .. ")" }
    end

    --
    -- Restart and Shut Down, at the bottom behind a separator.
    --
    -- BeOS put them here and so does every desktop since, which is reason
    -- enough on its own - but the separator is doing real work: everything
    -- above it starts something and these two end everything, and a menu
    -- that mixed the two would eventually be a machine somebody turned off
    -- while reaching for a calculator.
    --
    -- A *request to the window manager*, not an action taken here. That
    -- process holds `owns_procctl`; this one does not, and asking is the
    -- whole point - see `handlers.power`.
    --
    items[#items + 1] = { separator = true }

    items[#items + 1] = {
      text = "Restart",
      on_choose = function()
        say("restarting")
        fs.send("/app/wm", { type = "power", action = "restart" })
      end,
    }

    items[#items + 1] = {
      text = "Shut Down",
      on_choose = function()
        say("shutting down")
        fs.send("/app/wm", { type = "power", action = "off" })
      end,
    }

    win:open_menu(win.origin_x, win.origin_y + H, items)
end

--
-- Right-click the Kosmos button: what the menu *is*, rather than what is in
-- it.
--
-- Two items, and the second is the important one. The menu being a folder
-- of launchers is the whole design, and nothing on the screen said so -
-- somebody would have had to be told that `/home/Deskbar` exists before
-- they could move anything. Opening it puts the answer one right-click from
-- the thing it is about, which is where a person looks.
--
-- Reloading is here for the same reason it is a property rather than a
-- per-click read: the tree is read once, so something has to say when it
-- changed, and `setprop /app/Deskbar/menu reload` at a prompt is a poor
-- answer for somebody who just dragged a file in a window.
--
local function kosmos_context_menu()
  win:open_menu(win.origin_x, win.origin_y + H,
                {
                  {
                    text = "Reload Menus",
                    icon = "Prefs_Appearance",
                    on_choose = function()
                      read_sections()

                      local n = 0

                      for _, s in ipairs(sections) do n = n + #s.items end

                      say(("%d sections, %d items"):format(
                          #sections, n))
                      win.dirty = true
                    end,
                  },
                  {
                    text = "Open Deskbar Folder",
                    icon = "Folder_generic",
                    on_choose = function()
                      local ok, why = fs.send("/app/wm",
                                              { type = "launch",
                                                program = "tracker",
                                                args = DESKBAR })

                      say(ok and ("opened " .. DESKBAR)
                          or tostring(why))
                    end,
                  },
                })
end


--------------------------------------------------------------------------
-- The bar itself.
--------------------------------------------------------------------------

--
-- A gradient, lighter at the top, which is `topbar.lua`'s and is kept for
-- its reason: there is no gradient primitive and there should not be one,
-- but a bar is thirty-six rows, so thirty-six fills *is* the gradient, and
-- that is a bargain the one piece of chrome always on screen can have.
--
--
-- `k` per cent toward white, and **negative is toward black**.
--
-- The second half is what a pressed button on this bar is made of. It was
-- the kit's `sunken` face, which is a grey - correct for a dialog, and on a
-- yellow strip it reads as a hole rather than as the same surface pushed
-- in. A button on a coloured bar should be that colour, darker; anything
-- else looks like a different material.
--
local function lit(colour, k)
  local a = colour & 0xff000000
  local r = (colour >> 16) & 0xff
  local g_ = (colour >> 8) & 0xff
  local b = colour & 0xff

  if k >= 0 then
    r = r + ((255 - r) * k) // 100
    g_ = g_ + ((255 - g_) * k) // 100
    b = b + ((255 - b) * k) // 100
  else
    r = r + (r * k) // 100
    g_ = g_ + (g_ * k) // 100
    b = b + (b * k) // 100
  end

  return a | (r << 16) | (g_ << 8) | b
end

local KOSMOS_W = 12 + ICON + 8 + gfx.measure("Kosmos") + 12
local TASK_W = 190              -- a button per window, at most this wide
local GAP = 4

--
-- The three shades the bar is made of, as a ladder rather than three
-- numbers scattered through the drawing.
--
--   the strip      theme.tab, with the gradient over it
--   a button       a touch lighter than the strip
--   pressed        a shade darker *than the button*
--
-- The last one is the point: pressed has to be darker than the thing it is
-- a pressed version of, or the row of buttons stops looking like one set of
-- objects. Measuring it from the strip instead gave a pressed button that
-- was darker than the bar and lighter than its neighbours, which is how it
-- ended up looking like a hole.
--
local FACE = 14
local PRESSED = -20

--
-- Rounded corners, which is what Mac OS X put on a menu-bar highlight and
-- what a button on a coloured strip wants: a hard rectangle reads as a
-- panel bolted on, and the same shape with four pixels off each corner
-- reads as a highlight *of* the bar.
--
-- There is no rounded-rectangle primitive and there should not be one. A
-- fill is a C loop over a rectangle, which is the right shape for almost
-- everything here; a button is thirty-two rows, so thirty-two fills is the
-- rounding, and the bar redraws only when something changes rather than
-- every frame.
--
-- The corner is a table rather than arithmetic. Two numbers are exact, and
-- a circle worked out per row would be `math.sqrt` in a drawing path to
-- produce the same four numbers - with the difference that nobody could
-- see, from the code, what shape it draws.
--
local CORNER = { 2, 1 }

local function rounded(g, x, y, w, h, colour)
  for row = 0, h - 1 do
    local from_edge = math.min(row, h - 1 - row)
    local inset = CORNER[from_edge + 1] or 0

    if w > inset * 2 then
      g:fill(x + inset, y + row, w - inset * 2, 1, colour)
    end
  end
end

local bar = ui.view{ x = 0, y = 0, w = win.w, h = win.h }

--
-- Where the indicators start, worked out while drawing and remembered so
-- that `mouse` can test against the same numbers. Laid out from the right
-- edge inward, so the clock does not move when a one-digit hour becomes
-- two - the thing that makes a menu-bar clock look unsteady.
--
local right_x = win.w

--
-- How wide a window's button is, and how many fit.
--
-- Everything between the Kosmos button and the indicators, shared out. A
-- taskbar that lets buttons run under the clock is one where the last
-- application you opened is the one you cannot reach.
--
--
-- **Every window gets a button, however many there are.**
--
-- They share what is between the Kosmos end and the indicators, capped at
-- `TASK_W` so that two windows do not each get half the screen. Past about
-- eight they start shrinking; past about thirty-four there is no room for a
-- title and they become the icon alone; past that they are slivers.
--
-- A sliver is ugly and it is *reachable*, which is the property that
-- matters. This first had a floor and a `break`: buttons stopped shrinking
-- at icon width and any window that did not fit got no button. That is a
-- taskbar which silently hides the thing you are looking for, and the one
-- promise it makes is that everything running is on it.
--
-- Not solved here and worth naming: at some width a person cannot tell the
-- slivers apart. Windows groups by application and BeOS's Deskbar stacked
-- them; either would be a real answer and neither is one this needs yet.
--
local function task_spans()
  local out = {}
  local room = right_x - KOSMOS_W - GAP * 2
  local n = #running

  if n == 0 or room < n then return out end

  local each = math.min(TASK_W, room // n - GAP)

  if each < 1 then each = 1 end

  local x = KOSMOS_W + GAP

  for _, w_ in ipairs(running) do
    out[#out + 1] = { w_ = w_, x = x, w = each }
    x = x + each + GAP
  end

  return out
end

function bar:draw(g)
  for row = 0, self.h - 1 do
    local k = (38 * (self.h - 1 - row)) // (self.h - 1)

    g:fill(0, row, self.w, 1, lit(theme.tab, k))
  end

  local ty = (self.h - gfx.font.h) // 2
  local iy = (self.h - ICON) // 2

  --
  -- The Kosmos button, at the left because that is where a person looks for
  -- the thing that starts other things, and lit while its menu is open so
  -- the bar says where the menu came from.
  --
  if self.menu_open then
    rounded(g, 2, 2, KOSMOS_W - 4, self.h - 4,
            lit(lit(theme.tab, FACE), PRESSED))
  end

  g:icon(12, iy, "App_Deskbar.png", ICON)
  g:text(12 + ICON + 8, ty, "Kosmos", theme.tab_text)

  --
  -- The right-hand end: the clock, the date, the volume and the network,
  -- laid out from the edge inward.
  --
  --------------------------------------------------------------------
  -- The right-hand end, laid out from the edge inward.
  --
  -- **One gap between things, and it is generous.** This was eight pixels
  -- between the icons and twelve around the clock, with a question mark
  -- floating between two of them, and it read as clutter - which is what
  -- an indicator area must not be, because the whole job of one is to be
  -- glanced at rather than read.
  --
  -- Inward from the right so the clock does not move when a one-digit hour
  -- becomes two, which is the thing that makes a menu-bar clock look
  -- unsteady.
  --
  -- The date sits close to the time because they are one thing - a moment -
  -- and everything else is `PAD` from its neighbour.
  --------------------------------------------------------------------
  local PAD = 16
  local KERN = 10             -- inside a group: the date and its time

  local now = clock.now()
  local time = clock.time_string(now)
  local date = clock.date_string(now)

  local x = self.w - PAD - gfx.measure(time)

  g:text(x, ty, time, theme.tab_text)

  x = x - KERN - gfx.measure(date)
  g:text(x, ty, date, theme.tab_text)

  --
  -- The volume, and **nothing is drawn when the machine cannot answer.**
  --
  -- `topbar.lua` refused to draw indicators for subsystems that did not
  -- exist, on the grounds that a picture which lies about what the system
  -- knows is worse than a gap. What decides it now is the kernel rather
  -- than this file: `needs audio` grants nothing on a board with no sound
  -- card, so `/dev/audio` is not in this namespace and `audio.stats()` says
  -- so. The rule is the same one; it is enforced instead of remembered.
  --
  if volume_now() then
    x = x - PAD - ICON
    g:icon(x, iy, "Misc_Speaker.png", ICON)
    self.volume_x = x
  else
    self.volume_x = nil
  end

  if network_now() then
    x = x - PAD - ICON
    g:icon(x, iy, "Prefs_Network.png", ICON)
    self.network_x = x
  else
    self.network_x = nil
  end

  --
  -- The battery, and **it has no reading in it.**
  --
  -- This machine cannot read one: there is no driver, and the ThinkPad's
  -- embedded controller is its own piece of work - `roadmap.md` has it
  -- third in the agreed order, starting with getting the DSDT off the
  -- machine. So the picture is drawn and a question mark is drawn beside
  -- it, which is the part that says so: the battery alone would read as a
  -- charge, and the battery with a query reads as "nobody has asked".
  --
  -- **It must not become a number.** A battery drawn at 72% on a machine
  -- that cannot read one is indistinguishable from one that works, which is
  -- exactly the failure the rule above exists to prevent. When the reading
  -- arrives it replaces the query and nothing else here moves, which is the
  -- other reason to draw the shape now.
  --
  do
    local qw = gfx.measure("?")

    x = x - PAD - ICON - 4 - qw
    self.battery_x = x

    g:icon(x, iy, "App_PowerStatus.png", ICON)
    g:text(x + ICON + 4, ty, "?", theme.tab_text)
  end

  --
  -- How busy the machine is, and how much of its memory is spoken for.
  --
  -- Two small meters rather than numbers: at this size a figure is four
  -- characters somebody has to read, and a bar is a thing you notice out of
  -- the corner of an eye - which is the whole job of an indicator. Monitor
  -- is where the numbers live, and clicking these opens it.
  --
  -- Drawn only once there is something to say. Busy is a difference between
  -- two readings, so on the first pass there is nothing here, and a gap is
  -- the honest shape of "not yet".
  --
  local busy = load_now()
  local used, total = memory_now()

  if busy or used then
    local mw = 28

    x = x - PAD - mw
    self.meters_x = x

    local top = (self.h - 19) // 2

    if busy then
      g:fill(x, top, mw, 8, theme.sunken)
      g:fill(x, top, (mw * busy) // 100, 8, theme.accent)
    end

    if used and total and total > 0 then
      g:fill(x, top + 11, mw, 8, theme.sunken)
      g:fill(x, top + 11, (mw * used) // total, 8, theme.accent)
    end
  else
    self.meters_x = nil
  end

  right_x = x - PAD

  --
  -- And a button per window, with the application's own picture on it.
  --
  -- Sunken while its window has the focus, which is the kit's own sentence
  -- for "this one is pressed in" - `ui.md` 16.8b - and dimmed while it is
  -- minimised, because a window that is put away is still a thing you have
  -- rather than a thing on the screen.
  --
  for _, s in ipairs(task_spans()) do
    local w_ = s.w_

    --
    -- The bar's own colour, darker when this window has the focus and a
    -- touch lighter when it does not. The edges do the rest: sunken for the
    -- one you are in, nothing for the others.
    --
    -- Not the kit's grey faces. A grey rectangle on a yellow strip is a
    -- different material, which reads as a hole rather than as this surface
    -- pressed in - and the one thing a taskbar button has to say is "this
    -- is the window you are in".
    --
    local face = lit(theme.tab, FACE)

    --
    -- No bevel. The shade says which one you are in and the rounding says
    -- it is part of the bar; a one-pixel sunken edge around a rounded shape
    -- is a rectangle drawn around a rounded rectangle, which is the two
    -- vocabularies at once.
    --
    if w_.focused then
      rounded(g, s.x, 2, s.w, self.h - 4, lit(face, PRESSED))
    elseif not w_.hidden then
      rounded(g, s.x, 2, s.w, self.h - 4, face)
    end

    --
    -- The picture, and then the title in whatever is left of the button.
    --
    -- Both are skipped when there is no room rather than drawn over the
    -- edge: `g:icon` clips to the view and would otherwise spill a picture
    -- across the next button, and a title with two pixels to live in is
    -- noise. What is left is a coloured block you can still click, which is
    -- the promise.
    --
    if s.w >= ICON + 8 then
      g:icon(s.x + 4, iy, w_.icon .. ".png", ICON)
    end

    --
    -- The title, cut to what is left. `gfx.measure` rather than a character
    -- count, because the font is proportional and a count would cut "Web
    -- browser" and "MMMMMMMMMM" in the same place.
    --
    local room = s.w - ICON - 12
    local text = tostring(w_.title or "")

    if room >= gfx.font.w then
      while #text > 1 and gfx.measure(text) > room do
        text = text:sub(1, #text - 1)
      end

      g:text(s.x + ICON + 8, ty, text, theme.tab_text)
    end
  end
end

--------------------------------------------------------------------------
-- What a click on the bar does.
--------------------------------------------------------------------------

function bar:mouse(action, x, y)
  local _ = y

  if action ~= "press" then return false end

  if x < KOSMOS_W then
    open_kosmos_menu()
    return true
  end

  if self.volume_x and x >= self.volume_x and x < self.volume_x + ICON then
    fs.send("/app/wm", { type = "launch", program = "/bin/mixer.lua" })
    return true
  end

  if self.network_x and x >= self.network_x and x < self.network_x + ICON then
    fs.send("/app/wm", { type = "launch", program = "/bin/network.lua" })
    return true
  end

  if self.meters_x and x >= self.meters_x and x < self.meters_x + 26 then
    fs.send("/app/wm", { type = "launch", program = "/bin/sysmon.lua" })
    return true
  end

  if x >= right_x then
    -- The clock and the date, which are a control: a clock showing the
    -- wrong time with no way to say so from the clock is the first thing
    -- anybody hits on a new machine.
    fs.send("/app/wm", { type = "launch", program = "/bin/datetime.lua" })
    return true
  end

  for _, s in ipairs(task_spans()) do
    if x >= s.x and x < s.x + s.w then
      --
      -- **A second click on the window you are already in puts it away.**
      --
      -- Which is what every taskbar does and is the only gesture on this
      -- bar that is not obvious from looking at it. The alternative - a
      -- click always raises - makes the button under a focused window do
      -- nothing at all, and a control that does nothing is worse than one
      -- that does something you have to learn once.
      --
      -- `focused` and `hidden` both come from the window manager rather
      -- than from anything remembered here: a second memory of one fact is
      -- a second thing to be wrong.
      --
      local w_ = s.w_
      local what = (w_.focused and not w_.hidden) and "minimise" or "raise"

      fs.send("/app/wm", { type = what, window = w_.handle })
      return true
    end
  end

  return false
end

--
-- Right-click the Kosmos end of the bar: what the menu *is*, rather than
-- what is in it. Anywhere else on the bar has nothing to say yet.
--
function bar:on_context(x, _)
  if x < KOSMOS_W then
    kosmos_context_menu()
    return true
  end

  return false
end

win:add(bar)

--------------------------------------------------------------------------
-- Startup items.
--
-- What `/bin/startup` ticked, opened once, here, after the Deskbar's own
-- window exists.
--
-- **Here rather than anywhere earlier on the boot path**, and that is the
-- whole point of the feature living in this file. `init.lua` runs one
-- program from a firmware option and refuses to read a setting off the
-- disk, because a machine that will not reach a prompt because of something
-- written in a file is a machine you cannot fix from the prompt. Nothing
-- here changes that: by the time this line runs there is a shell, a window
-- manager and a Deskbar, so the worst a bad entry can do is open a window
-- that dies - and the panel that unticks it is two clicks away.
--
-- Started through the same `launch` message the menu sends, so an item that
-- opens at startup and the same item chosen by hand are the same thing.
--------------------------------------------------------------------------
do
  -- The list, or what a machine nobody has told opens. `/lib/startup.lua`
  -- holds both so that this and the panel cannot disagree about it.
  local items = use("/lib/startup.lua").items()

  local started = 0

  for _, name in ipairs(items) do
    name = tostring(name)

    -- Only what this Deskbar would list. A name in the file that is no
    -- longer in `/bin` is a stale tick, not a reason to send the window
    -- manager a program it cannot find.
    local known = false

    for _, have in ipairs(launchable) do
      if have == name then known = true break end
    end

    -- Narrated, because this is the step nobody could see. Four windows
    -- opening at login is four `launch` messages from here, and when one of
    -- them does not arrive there is nothing on the screen to say which - so
    -- each is announced with what came back. `log deskbar` at the prompt
    -- reads them after the desktop has been left.
    if not known then
      print(("deskbar: %s is not in /bin, so it was not started"):format(name))
    else
      local sent = fs.send("/app/wm", { type = "launch", program = name })

      print(("deskbar: launch %s -> %s"):format(name, tostring(sent)))

      if sent then
        started = started + 1
      end
    end
  end

  if started > 0 then
    say(("started %d at login"):format(started))
  end
end

win:run()
