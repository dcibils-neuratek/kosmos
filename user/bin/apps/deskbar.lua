-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Deskbar
-- kosmos: name Deskbar
-- kosmos: needs screen network
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
-- **`network` is for the indicators, and it is a change of position worth
-- naming.** This used to declare `needs screen` and nothing
-- else, and the paragraph below said why - launching goes through the
-- window manager so that reaching the Deskbar is not reaching everything.
-- That is still true of *power*: Restart and Shut Down are a request this
-- sends, because `processes` stays with the window manager. What changed is
-- that the bar is where a person now manages the machine from, and a bar
-- that cannot see the volume cannot show it. Reading state is not holding
-- power, and the two are kept apart on purpose.
--
-- It does not widen what this can reach on a machine that lacks the
-- hardware: `init.lua` grants `network` only when there is a card. The
-- volume is read from `/Devices/audio` as any client reads it; this
-- declared `needs audio` for it too while that word was the sound device,
-- and since it became the audio band (`roadmap.md` 4i) there is nothing
-- here that makes sound to want it.
--
--------------------------------------------------------------------------
-- Where the two lists come from, and why neither is a list this program
-- keeps.
--
-- **What is running** is the window manager's own list of windows, not
-- `/Running`. The registry holds applications that *registered*, which means
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
-- **What can be run** is `/Home/Deskbar`: a folder per section, holding
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

local ui = use("/Kosmos/Libraries/ui.lua")
local menudata = use("/Kosmos/Libraries/deskbarmenu.lua")
local files = use("/Kosmos/Libraries/files.lua")
local types = use("/Kosmos/Libraries/filetypes.lua")
local clock = use("/Kosmos/Libraries/clock.lua")
local notify = use("/Kosmos/Libraries/notify.lua")
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
-- launcher file exists.** `/Home/Deskbar` holds a folder per section, each
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
-- **And it is two trees** (`roadmap.md` 6zd): `/Kosmos/Deskbar` is the menu
-- as it ships, laid out from each application's `kosmos: section` and
-- `kosmos: icon` by the store that serves them, and `/Home/Deskbar` holds
-- only what the person made - their launchers, their folders, Doom with
-- their own arguments. Shown merged (`deskbarmenu.lua`). It used to be one:
-- the shipped menu was copied into every home, where it went stale, went to
-- other machines with the home, and could not be told from the person's.
--
-- **No launcher, no entry.** There is deliberately no fallback that lists a
-- program with no launcher, and no ledger of programs seen before: both
-- were considered and both exist only to patch a fallback. A program built
-- later is invisible until somebody makes a launcher for it, which is the
-- rule working rather than a gap in it - `/bin` is in the namespace, so
-- Tracker can open it and a launcher can be made from there.
--------------------------------------------------------------------------

local DESKBAR = "/Home/Deskbar"
local SHIPPED = "/Kosmos/Deskbar"

--
-- What `/bin` can start, which is a different question from what the menu
-- lists and is still worth asking.
--
-- Startup items name a *program*, and are checked against this rather than
-- against the menu: "open this at login" and "show this in the menu" are
-- two choices, and an item can reasonably be one without the other.
--
local programs = {}

do
  for _, file in ipairs(fs.list("/Kosmos/Apps") or {}) do
    local attrs = fs.getattr("/Kosmos/Apps/" .. file)

    if attrs and attrs.kind == "application" then
      local short = file:gsub("%.lua$", "")

      -- The Deskbar does not list itself. It is not something you start.
      -- `section none`: a window something else opens with a file in it -
      -- Info - which has nothing to show opened on its own from a menu.
      if short ~= "deskbar" and attrs.section ~= "none" then
        programs[short] = attrs
      end
    end
  end
end

--
-- **What the seed left, to the Trash once** (`roadmap.md` 6zd).
--
-- Until 28 September this Deskbar copied a launcher for every application
-- into `/Home/Deskbar` - the whole menu, the first time it found no folder,
-- and every new application after - and kept `.seeded`, the record of what
-- it had given. With the menu shipping in `/Kosmos/Deskbar` those copies
-- are the menu twice. Diego: "Send to the trash all seeded". So on a home
-- that has the record, every launcher the seed made goes to the Trash -
-- a folder of nothing else whole, so the Trash keeps the menu's shape
-- (`deskbarmenu.seed_leftovers`) - and the record after them, which is what
-- makes it once. One a person changed goes too, and can be dragged back.
--
-- To the Trash, never deleted: what goes is the person's to have back.
--
local SEEDED = DESKBAR .. "/.seeded"

local function retire_seed()
  local record = fs.read(SEEDED)

  if type(record) ~= "table" or type(record.programs) ~= "table" then return end

  local seeded = {}

  for _, short in ipairs(record.programs) do seeded[tostring(short)] = true end

  -- The Trash is Tracker's to make, and this may start before Tracker ever
  -- has on this home.
  for _, dir in ipairs({ files.parent(files.TRASH), files.TRASH }) do
    if not fs.getattr(dir) then fs.send(dir, { type = "mkdir" }) end
  end

  local moved = 0

  for _, path in ipairs(menudata.seed_leftovers(fs, DESKBAR, seeded)) do
    local name = files.free_name(files.TRASH, path:match("([^/]+)$"))
    local ok, why = name and files.move(path, files.join(files.TRASH, name))

    if ok then
      moved = moved + 1
    else
      print(("deskbar: %s stayed: %s"):format(path, tostring(why)))
    end
  end

  local name = files.free_name(files.TRASH, ".seeded")
  local ok, why = name and files.move(SEEDED, files.join(files.TRASH, name))

  if not ok then
    print("deskbar: the seed's record stayed: " .. tostring(why))
  end

  print(("deskbar: %d of the seed's launchers and folders went to the Trash")
        :format(moved))
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
--   setprop /Running/Deskbar/menu reload
--
-- No new mechanism and no new program: `ui.window` registers this window
-- with `/Running` and answers for its properties, and `setprop` is the general
-- four-line program that writes one. The editor sends the same thing after
-- it changes a launcher, so the menu is right without anybody being told to
-- do anything.
--
local sections = {}

--
-- A launcher to a program that is gone is not shown: `programs` is what
-- `/Kosmos/Apps` declared at the start, and a path outside it is somebody's
-- own and trusted. `/bin/<name>.lua` is how a launcher made before 27
-- September says the same (`ns.program` starts it from its new place).
--
local function exists(program)
  local short = program:match("^/[Kk][Oo][Ss][Mm][Oo][Ss]/[Aa][Pp][Pp][Ss]/([^/]+)%.lua$")
                or program:match("^/[Bb][Ii][Nn]/([^/]+)%.lua$")

  if short then return programs[short] ~= nil end
  if not program:find("/", 1, true) then
    return programs[(program:gsub("%.lua$", ""))] ~= nil
  end

  return true
end

--
-- Three layers: the menu that ships, the applications installed in
-- `/Home/Apps` (`docs/elf.md` step 5) beside it, and the person's own on top.
--
local function read_sections()
  local installed = menudata.installed(types.installed(fs), types.declared)

  sections = menudata.merge_sections(
    menudata.merge_sections(menudata.sections(fs, SHIPPED, exists), installed),
    menudata.sections(fs, DESKBAR, exists))
end

retire_seed()

-- Somewhere for the person's own, which a menu that only ships would never
-- make: a folder that exists once you think to make one is one nobody
-- makes, as Tracker says of `/Home/Desktop`.
if not fs.getattr(DESKBAR) then fs.send(DESKBAR, { type = "mkdir" }) end

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
-- **32 pixels, with 24-pixel icons, and not a setting** (`theme.metrics`,
-- `roadmap.md` 5v). An icon large enough to recognise is what makes a row
-- of buttons scannable rather than a row of words. It was 36 with the
-- icons at 32, because 32 was the only size the image carried and nothing
-- scaled one; then 36, 44 or 52, chosen in Appearance. Diego, 22 September,
-- on the ThinkPad: "Taskbar size should not be changeable let's make it
-- fixed at 32". A 32-pixel icon in a 32-pixel bar touches both edges, so
-- the icons are 24, averaged down from Haiku's 64-pixel ones (`gc:icon`),
-- with four pixels above and below.
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
-- namespace at all - `/Devices/audio` is not denied, it is absent - so the
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
  local ok, got = pcall(use, "/Kosmos/Libraries/audio.lua")

  audio_lib = ok and got or nil
end

--
-- The master gain, 0 to 256, or nil when there is no audio to ask about.
--
local function volume_now()
  if not audio_lib then return nil end

  local ok, stats = pcall(audio_lib.stats)

  if not ok or not stats or not stats.master then return nil end

  return stats.master, stats.master_muted
end

--
-- Whether there is a card, which is the one thing about the network a bar
-- can say in an icon. `network.lua` is where the rest of it lives, and
-- clicking the icon opens it.
--
-- `fs.net_info("/Network")` is what that program asks, and it is the only thing
-- that answers: there is no `/Devices/net`. The devices server serves `cpu`,
-- `clock`, `screen`, `keyboard`, `memory` and `cores`, and the network is
-- not one of them - it is reached through the capability rather than
-- through a device file, which is why `needs network` is the whole of the
-- access check.
--
--
-- How busy the machine is, as a percentage, and how much memory is in use.
--
-- The same two readings `monitor.lua` draws, from the same two device
-- files: `/Devices/kernel` counts idle and busy ticks and `/Devices/memory` says
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
  local ok, k = pcall(fs.read, "/Devices/kernel")

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
  local ok, m = pcall(fs.read, "/Devices/memory")

  if not ok or type(m) ~= "table" or not m.total_mb then return nil end

  return m.total_mb - m.free_mb, m.total_mb
end

--
-- The battery, from `/Devices/battery`, or nil when the machine reads none -
-- the devices server lists that node only when the board has a reading.
--
local function battery_now()
  local ok, b = pcall(fs.read, "/Devices/battery")

  if not ok or type(b) ~= "table" or b.present ~= 1 or not b.percent then
    return nil
  end

  return b
end

--
-- A card, not merely a stack: the stack answers `net_info` with no card to
-- drive, and says so in `card`. This read only that the stack answered, so
-- the picture was drawn on every PC, card or not - found on 27 September
-- by the x86 suite's first machine with `-nic none`, once "offline" was a
-- thing the bar says (`roadmap.md` 6zl).
--
local function network_now()
  local ok, info = pcall(fs.net_info, "/Network")

  if not ok or type(info) ~= "table" or not info.card then return nil end

  return info
end

--
-- **What the bar last heard, which is all it draws from** (`testing.md`
-- 18.345; Diego, 2 October: "all that needs to be programmed async so it
-- does not wait or hang waiting for network or anything").
--
-- The draw asked five servers on every repaint - the clock, the sound, the
-- network, the battery, the processor and memory - so a repaint was only
-- as quick as the slowest of them. On 2 October the slowest was the network
-- stack, held four seconds and more at a time by a card that would not
-- send, and the bar's first picture came 23 seconds late, its menu with it:
-- a click is a repaint too. Now the questions are asked on the bar's own
-- second (`clockwork:tick`), a repaint draws what was heard, and an
-- indicator nobody has answered yet is a gap - the honest picture of "not
-- yet", as the meters already were. The first second is asked at once,
-- after the first picture rather than before it.
--
-- Asking is still a call, and a server that never answered would still hold
-- the tick; that is what a call with a deadline is for (`roadmap.md`), and
-- a server that answers at once is the rule the network stack's card driver
-- was brought back to the same day.
--
local heard = {}

--
-- **The notifications' history, from the clock** (`roadmap.md`,
-- *Notifications*, step 2): a press on the clock opens it - the
-- `notifications` program's panel - and a dot beside the date says
-- something has arrived since it was last opened. What has arrived is the
-- server's newest number, asked on the bar's second with the rest; what
-- was looked at is the newest when the panel was last opened, or when this
-- bar started, so a Deskbar starting again does not call old news new.
--
local looked = nil

local function open_history()
  looked = heard.newest or looked
  fs.send("/Running/wm", { type = "launch", program = "/Kosmos/Apps/notifications.lua",
                           args = "panel" })
  print("deskbar: the notifications' history asked for")
end

local function news()
  return heard.newest and looked and heard.newest > looked
end

local function listen()
  heard.at = sys.ticks()
  heard.newest = notify.newest()
  looked = looked or heard.newest
  heard.now = clock.now()
  heard.level, heard.muted = volume_now()
  heard.network = network_now() and "wired" or "offline"
  heard.battery = battery_now()
  heard.busy = load_now()
  heard.used, heard.total = memory_now()
end


local H = theme.metrics.deskbar
local ICON = 24

-- The indicators' line glyphs (`docs/statusicons.html`): 19 of the sizes
-- `tools/lineicons.py` renders, in the 32-pixel bar.
local LINE = 19
local W = sw

--
-- **Where the bar is** (`roadmap.md`, a dock at the bottom; Diego, 3
-- October: "an appearance setting to place the taskbar on the bottom
-- center"): across the top, as it has been, or a dock at the foot of the
-- screen with a strip across the top for the time and the indicators -
-- Appearance's `bar`, the dock floating unless `dock` says the whole
-- width. `--bar dock` or `--bar top` says so for one start, as a test asks.
--
local appearance = fs.read("/Home/Preferences/appearance")

if type(appearance) ~= "table" then appearance = {} end

local asked_bar = tostring(args or ""):match("%-%-bar%s+(%a+)")
local asked_dock = tostring(args or ""):match("%-%-dock%s+(%a+)")
local DOCKED = (asked_bar or appearance.bar) == "dock"
local FLOATING = (asked_dock or appearance.dock) ~= "whole"
local dock = DOCKED and use("/Kosmos/Libraries/dock.lua") or nil
local topstrip = nil              -- the dock's strip across the top
local kosmos_at = nil             -- where the dock drew its Kosmos button, in it

--
-- **The launcher**, `launchpad`, which the dock's Kosmos button opens as a
-- grid above it (`roadmap.md`, a dock at the bottom, step 4). Open while
-- the window manager lists it; asked for at the press, so the button lights
-- then rather than when the grid arrives (`ui.md`, instant feedback).
--
local launcher_open = false
local dock_open_launcher = nil    -- the dock's Kosmos button, pressed (with the dock)
local launcher_handle = nil       -- its window, while it is open
local launcher_asked = nil        -- the counter when the button was pressed

local function is_launcher(program)
  local p = tostring(program or "")

  return p == "launchpad" or p:match("/launchpad%.lua$") ~= nil
end

if DOCKED then H = dock.H end

local win, err

if DOCKED then
  -- The dock: as wide as what is in it once that is known (`dock_frame`),
  -- or the whole width; blended, so its corners and the gap round it are
  -- the screen.
  win, err = ui.window{ title = "Deskbar", w = FLOATING and 400 or W, h = H,
                        strip = "bottom", floating = FLOATING or nil, blend = true }

  if win then
    topstrip = ui.window{ title = "Deskbar strip", w = W, h = dock.STRIP_H,
                          strip = "top", blend = true }
  end

  print(("deskbar: a dock, %s"):format(FLOATING and "floating" or "the whole width"))
else
  win, err = ui.window{ title = "Deskbar", w = W, h = H, strip = "top" }
end

if not win then
  print("deskbar: " .. tostring(err))
  return
end

--
-- `setprop /Running/Deskbar/menu reload` - the menu, read again.
--
-- Reading it costs about fifty messages to the disk server, so it happens
-- when somebody says the tree changed rather than on every click. Anything
-- that edits a launcher sends this afterwards; a person who moved one in
-- Tracker uses Reload Menus on the Kosmos button, and reading it back says
-- how many items are there now, which is how you tell a menu that did not
-- notice from a launcher that was not made. `Deskbar` with the capital is
-- the window's title, which is the name `ui.window` registers it under.
--
--
-- **And what is in it**: each section's rows by name, a folder with a
-- slash after it and the person's own with a star (`roadmap.md` 6zd) - an
-- item of theirs in `/Home/Deskbar` rather than the menu as it ships. Which
-- is how a menu that did not merge would be told from one that did.
--
win:publish("menu",
  function()
    local n, said = 0, {}

    for _, section in ipairs(sections) do
      local rows = {}

      n = n + #section.items

      for _, item in ipairs(section.items) do
        rows[#rows + 1] = item.name .. (item.folder and "/" or "")
          .. ((tostring(item.path or ""):sub(1, #DESKBAR + 1) == DESKBAR .. "/")
              and "*" or "")
      end

      said[#said + 1] = section.name .. ": " .. table.concat(rows, ", ")
    end

    return ("%d sections, %d items; %s"):format(#sections, n,
                                                table.concat(said, "; "))
  end,
  function()
    read_sections()
    win.dirty = true
  end)

--
-- **Another place for the bar**, told by Preferences as the setting changes
-- (`roadmap.md`, a dock at the bottom): a Deskbar started again with the
-- new words, and this one closed once the window manager has said yes - as
-- Groove starts itself again at another size. The words go on its command
-- line rather than through the file, because Preferences applies a setting
-- before it writes it.
--
--
-- **Asked for, then done from the loop**: the setter only notes the words,
-- so the write that asked is answered by a Deskbar still there to answer
-- it - closing from inside the setter left `setprop` holding an error from
-- a process that had gone. `--again` is what keeps the new one from opening
-- the login items a second time.
--
local leaving = nil         -- { bar, dock } once a new place has been asked for

local function again(bar_is, dock_is)
  local reply = fs.send("/Running/wm", { type = "launch", program = "deskbar",
                                         args = ("--bar %s --dock %s --again"):format(bar_is, dock_is) })

  if reply and reply.ok then
    print(("deskbar: again, the bar %s and the dock %s"):format(bar_is, dock_is))
    if topstrip then topstrip:close() end
    win:close()
  else
    print("deskbar: could not start again: " .. tostring(reply and reply.error))
  end
end

-- The place it already has asks for nothing: Preferences applying the
-- setting it shows is not a reason for the bar to blink. Nor does the
-- dock's width while the bar is at the top - it is kept for when it moves.
local function move_to(bar_is, dock_is)
  if bar_is == "top" and not DOCKED then
    FLOATING = dock_is ~= "whole"
    return
  end

  if bar_is == "dock" and DOCKED and (dock_is ~= "whole") == FLOATING then return end

  leaving = { bar_is, dock_is }
end

win:publish("bar",
  function() return DOCKED and "dock" or "top" end,
  function(v)
    move_to(tostring(v) == "dock" and "dock" or "top", FLOATING and "floating" or "whole")
  end)

win:publish("dock",
  function() return FLOATING and "floating" or "whole" end,
  function(v)
    move_to(DOCKED and "dock" or "top", tostring(v) == "whole" and "whole" or "floating")
  end)

--------------------------------------------------------------------------
-- What is running, told by the window manager when it changes.
--
-- The window manager's list of windows, not `/Running`: the registry holds
-- applications that *registered*, and a program that opens a window by
-- talking to the desktop directly has a window on screen and no
-- registration anywhere. What belongs on a taskbar is what is on the
-- screen, and the desktop is the only thing that knows that.
--
-- **Told, not asked on a clock.** This asked on its tick, which is once a
-- second, so a focus that moved anywhere but here - a click on a window,
-- Control-W Tab, a window opening - reached the bar up to a second late:
-- 290 to 1029 ms measured under QEMU, about 600 on average, and "like half
-- a second" on the ThinkPad. Every request for the list now says `watch`,
-- and the window manager posts a `windows` event whenever its answer would
-- be different (`tell_watchers` in `wm.lua`). The event says only that; the
-- list is asked for again, so the reply stays the one place it is written.
--
-- Sorted by handle, which is the order the windows opened in and never
-- changes. The reply comes back in *stacking* order, and stacking order
-- moves every time anything is raised - including by the click about to
-- land on this bar. `procs` learned the same thing the same way: "you aim
-- at one and end another".
--------------------------------------------------------------------------

local running = {}          -- { handle, title, icon, focused, hidden }

--
-- **And what is starting** (`docs/launching.html`; Diego: "I like to try the
-- app breathing launcher indicator"). The window manager lists a program
-- from the request until its first window (`starting` in `wm.lua`), and the
-- bar gives it a button at the end of the row, its picture breathing and
-- its name dimmed, until the window's own button takes its place - so a
-- program that is slow to start is plainly on its way, and a second click
-- has nothing to ask for. A failure is said on the button for the three
-- seconds the window manager keeps it. Entries have `starting` set and no
-- `handle`; a click on one does nothing.
--
local BREATH = 1.2                          -- seconds, in and out
local counter_hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000
local tick_hz = (sys.info() or {}).tick_hz or 100

-- The name a starting program is shown by: an installed application's
-- folder - `/Home/Apps/Doom/doom.lua` is Doom - or its file's name.
local function title_of(program)
  local folder = tostring(program):match("/Apps/([^/]+)/[^/]+%.lua$")

  if folder then return folder end

  local name = tostring(program):match("([^/]+)%.lua$") or tostring(program)

  return name:sub(1, 1):upper() .. name:sub(2)
end

local function any_starting()
  for _, w_ in ipairs(running) do
    if w_.starting and not w_.failed then return true end
  end

  return false
end

-- How much of a starting program's picture shows now: 35 to 100 per cent,
-- a breath every `BREATH` seconds, the mockup's.
local function breath()
  local t = (sys.ticks() / counter_hz) % BREATH / BREATH

  return math.floor(255 * (0.35 + 0.65 * (0.5 + 0.5 * math.cos(2 * math.pi * t))))
end

-- Woken twelve times a second while something breathes, and not otherwise:
-- a bar that animates for nothing is a bar that keeps the machine awake.
local function pace_breathing()
  win.poll_wait_ticks = any_starting() and math.max(1, tick_hz // 12) or nil
end

--
-- **Pressed in means the window you are in, and a minimised window is not
-- one.** The window manager's `focused` is the top of its stack, and a
-- window put away by its own minimise box stays at the top - so the bar drew
-- a minimised window as the selected one. The click already knew better:
-- a second click minimises only a window that is focused *and* showing.
-- One predicate for both, so the picture and the gesture cannot disagree.
--
local function selected(w_)
  return w_.focused and not w_.hidden
end

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
    print(("deskbar: %s draws as %s"):format(program, icon_of[program]))
  end

  return icon_of[program]
end

--
-- The list, asked for again, and whether anything the bar draws from it
-- changed - which is what a caller hands back to the kit, so an answer that
-- moved nothing repaints nothing.
--
-- `watch` on every request rather than on the first, because it costs a
-- field and the line that asks then says what it is asking for.
--
local function refresh()
  local reply = use("/Kosmos/Libraries/wmproto.lua").windows(win.handle)
  local list = {}

  --
  -- Chrome is not something that is running. The desktop and this bar are
  -- windows, because everything here is, but neither is an application you
  -- started or one you can switch to. The window manager marks them.
  --
  --
  -- **Nor is the launcher**: a panel that comes and goes, which lights the
  -- Kosmos button while it is open rather than taking a place of its own.
  --
  local launcher = false

  launcher_handle = nil

  for _, w_ in ipairs(reply and reply.windows or {}) do
    if is_launcher(w_.program) then
      launcher = true
      launcher_handle = w_.handle
    elseif not w_.chrome then
      list[#list + 1] = w_
    end
  end

  table.sort(list, function(a, b) return a.handle < b.handle end)

  local rows = {}

  for i, w_ in ipairs(list) do
    rows[i] = { handle = w_.handle, title = w_.title,
                icon = picture(w_.program), program = w_.program,
                focused = w_.focused, hidden = w_.hidden }
  end

  -- What is starting, after what is running: where its window's button
  -- will be when it opens, so nothing jumps.
  for _, s in ipairs(reply and reply.starting or {}) do
    if is_launcher(s.program) then
      launcher = true
      goto next_starting
    end

    rows[#rows + 1] = { starting = true, program = s.program,
                        title = title_of(s.program), icon = picture(s.program),
                        failed = s.failed }

    if s.failed then
      print(("deskbar: %s did not start: %s"):format(title_of(s.program),
            tostring(s.failed)))
    end

    ::next_starting::
  end

  local changed = (#rows ~= #running)

  if launcher ~= launcher_open then
    launcher_open = launcher
    changed = true

    if launcher then launcher_asked = nil end
  end

  for i, row in ipairs(rows) do
    local was = running[i]

    if not was or was.handle ~= row.handle or was.title ~= row.title
       or was.focused ~= row.focused or was.hidden ~= row.hidden
       or was.program ~= row.program or was.failed ~= row.failed then
      changed = true

      if row.starting and not row.failed then
        print(("deskbar: %s is starting"):format(row.title))
      end
    end

    running[i] = row
  end

  for i = #running, #rows + 1, -1 do running[i] = nil end

  pace_breathing()

  return changed
end

--
-- **The clock and the meters, which is all the tick is for now.**
--
-- Having a `tick` is what puts a window on the kit's once-a-second repaint
-- (`window:add` in `ui.lua`), and that repaint is what moves the clock, the
-- processor and memory meters, and the Kosmos end after its menu closes.
-- The tick used to ask for the list of windows too, and that is what made
-- the bar a second late.
--
-- **It is where the bar listens** (`heard`, above): the indicators' servers
-- are asked here, once a second, and never from the draw. It was empty, and
-- the whole bar is still repainted every second whether or not the minute
-- or a meter moved.
--
local clockwork = ui.view{ x = 0, y = 0, w = 0, h = 0 }

function clockwork:tick()
  listen()
  pace_breathing()
end

win:add(clockwork)

refresh()


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

  local ok, why = fs.send("/Running/wm", { type = "launch",
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
    text = menudata.shown(item),
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

      --
      -- **The button first, from what this row already knows** - its name
      -- and its picture - and then the request, not waited for: the window
      -- manager answers at once and says in its list what happened, so this
      -- bar is free to breathe while the program starts (`docs/launching.html`).
      --
      local already = false

      for _, w_ in ipairs(running) do
        if w_.starting and w_.program == item.program then already = true end
      end

      if not already then
        running[#running + 1] = { starting = true, program = item.program,
                                  title = menudata.shown(item),
                                  icon = item.icon or picture(item.program) }
        pace_breathing()
        win.dirty = true
      end

      local ok, why = fs.send("/Running/wm", { type = "launch",
                                           program = item.program,
                                           args = item.args, wait = false })

      say(ok and ("starting " .. item.program)
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
-- when a press lands left of `kosmos_w()`.
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
      icon = "App_Generic",   -- the three cubes (Diego: "like the ones used for demos like snes")
      on_choose = function()
        say("restarting")
        fs.send("/Running/wm", { type = "power", action = "restart" })
      end,
    }

    items[#items + 1] = {
      text = "Shut Down",
      icon = "App_Generic",   -- the three cubes (Diego: "like the ones used for demos like snes")
      on_choose = function()
        say("shutting down")
        fs.send("/Running/wm", { type = "power", action = "off" })
      end,
    }

    if DOCKED then
      -- Upwards, from above the dock's Kosmos button - where it was drawn,
      -- which along the whole width is the middle of the bar rather than its
      -- end (Diego's photograph, 3 October: "the apps menu appears way off
      -- the kosmos button").
      win:open_menu(win.origin_x + (kosmos_at or dock.PAD), win.origin_y - 6, items, true)
    else
      win:open_menu(win.origin_x, win.origin_y + H, items)
    end
end

--
-- **Opening the menu from somewhere else.**
--
-- The window manager binds the Windows key - Command on an Apple keyboard -
-- and it cannot reach into this process to open a menu, so it posts an
-- event and this opens it.
--
-- **Posted, never called.** The first attempt had the compositor `send` to
-- this process and the second had it `write` a property. Both are
-- synchronous: they wait for a reply, and a reply from the compositor's key
-- path is one the compositor is not running to receive. A single press of
-- the key stopped the whole desktop reading the keyboard, with no error and
-- no crash - just a machine that ignored you. `post` appends to this
-- window's queue and returns, which is what every mouse press already does.
--
win.on_event = function(_, ev)
  if ev.type == "menu" then
    -- With the bar a dock, the Windows key opens what its Kosmos button
    -- does - the launcher - as Googlebook's launcher key does.
    if dock_open_launcher then
      dock_open_launcher()
    else
      open_kosmos_menu()
    end

    return true
  end

  --
  -- The window manager saying its list of windows changed, which it posts
  -- because `refresh` asks with `watch`. Repainted only when something the
  -- bar draws from the list moved - a press on the bar has usually painted
  -- the answer already, and then this finds nothing to do.
  --
  if ev.type == "windows" then
    return refresh()
  end

  return false
end


--
-- Right-click the Kosmos button: what the menu *is*, rather than what is in
-- it.
--
-- Two items, and the second is the important one. The menu being a folder
-- of launchers is the whole design, and nothing on the screen said so -
-- somebody would have had to be told that `/Home/Deskbar` exists before
-- they could move anything. Opening it puts the answer one right-click from
-- the thing it is about, which is where a person looks.
--
-- Reloading is here for the same reason it is a property rather than a
-- per-click read: the tree is read once, so something has to say when it
-- changed, and `setprop /Running/Deskbar/menu reload` at a prompt is a poor
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
                      local ok, why = fs.send("/Running/wm",
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
-- Halfway between two colours, channel by channel: a name dimmed toward the
-- face it sits on, which reads as "not yet" in every look.
local function halfway(a, b)
  local out = 0xff000000

  for shift = 0, 16, 8 do
    local ca, cb = (a >> shift) & 0xff, (b >> shift) & 0xff

    out = out | (((ca + cb) // 2) << shift)
  end

  return out
end

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

--
-- **Measured when it is used, not when this file loads.**
--
-- It was a constant, computed at load: 12, the icon, 8, the width of the
-- word and 12. At that moment this process has never opened a window, so it
-- has never been told what the desktop's faces are - `ui.window` brings them
-- back in its reply and a theme change sends them again. The word was
-- therefore measured in the *kit's* default face and drawn in the
-- desktop's, and the two agreed only as long as they were the same face.
--
-- They stopped agreeing the day IBM Plex became the default (`roadmap.md`
-- 5): every button on the bar sat a few pixels from where the bar thought
-- it was, and a press near an edge found the wrong one. `gfx.measure` is a
-- call into C over a face the process already has, so asking each time
-- costs nothing worth naming.
--
local function kosmos_w()
  return 12 + ICON + 8 + gfx.measure("Kosmos") + 12
end
local TASK_W = 190              -- a button per window, at most this wide
local GAP = 4

--
-- The three shades the bar is made of, as a ladder rather than three
-- numbers scattered through the drawing.
--
--   the strip      theme.tab - the look's accent, as its windows' tabs
--                  are (`roadmap.md` 5y) - with the gradient over it
--   a button       a touch lighter than the strip
--   pressed        a shade darker *than the button*
--
-- The last one is the point: pressed has to be darker than the thing it is
-- a pressed version of, or the row of buttons stops looking like one set of
-- objects. Measuring it from the strip instead gave a pressed button that
-- was darker than the bar and lighter than its neighbours, which is how it
-- ended up looking like a hole.
--
--
-- **The face has to differ from the strip enough to have an edge.**
--
-- This was 14 per cent toward white, and on a saturated yellow that is
-- nearly the same colour: the button had no visible outline, so its corners -
-- rounded, then - were invisible too, and the whole row read as words printed on the
-- bar rather than as things you press. The corner was blamed first and the
-- corner was correct.
--
local FACE = 24
local PRESSED = -20

--
-- **A button on the bar is a plain rectangle.**
--
-- It was rounded - four pixels off each corner, "what Mac OS X put on a
-- menu-bar highlight" - and the strip's own top two corners were cut to
-- follow a Mac display's curve. Diego, 24 September, with every window now
-- rounded and framed: "i want to remove the rounded borders in the top bar
-- in the deskbar, just remove the rounded borders as i dont see it a good
-- idea anymore". The windows carry the curve now; the bar is the edge of
-- the screen, and a straight edge is what says so.
--
-- One `fill` a button, which is also the cheapest a button has ever been:
-- the rounded one was five (`roadmap.md` 5zz).
--

local bar = ui.view{ x = 0, y = 0, w = win.w, h = win.h }

-- As wide as the screen in points, which a new scale changes (`roadmap.md`
-- 5z): the bar is told its new size and follows it.
function win:on_resize(w, h)
  bar.w, bar.h = w, h
end

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
  local room = right_x - kosmos_w() - GAP * 2
  local n = #running

  if n == 0 or room < n then return out end

  local each = math.min(TASK_W, room // n - GAP)

  if each < 1 then each = 1 end

  local x = kosmos_w() + GAP

  for _, w_ in ipairs(running) do
    out[#out + 1] = { w_ = w_, x = x, w = each }
    x = x + each + GAP
  end

  return out
end

-- The battery's line glyph: the bolt while it charges, else how full. The
-- bar and the dock's strip both draw it.
local function battery_glyph(bat)
  return bat.state == "charging" and "battery-charging"
         or (bat.percent >= 80 and "battery-full")
         or (bat.percent >= 30 and "battery-medium")
         or "battery-low"
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
  --
  -- Lit while its menu is open, and **derived rather than remembered.**
  --
  -- This tested `self.menu_open`, which nothing ever set - so the Kosmos end
  -- never lit at all, however long the menu was up. The window already knows
  -- what menus it has open, and both of the bar's menus hang off this end,
  -- so the answer is `#win.menus` and there is no second copy of it to fall
  -- out of step.
  --
  local menu_up = #win.menus > 0

  if menu_up then
    g:fill(2, 2, kosmos_w() - 4, self.h - 4,
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

  local time = heard.now and clock.time_string(heard.now) or ""
  local date = heard.now and clock.date_string(heard.now) or ""

  local x = self.w - PAD - gfx.measure(time)

  g:text(x, ty, time, theme.tab_text)

  x = x - KERN - gfx.measure(date)
  g:text(x, ty, date, theme.tab_text)

  -- Something said since the history was last opened.
  if news() then
    x = x - 6 - 6
    g:fill_round(x, (self.h - 6) // 2, 6, 6, theme.accent, 3)
  end

  --
  -- The volume, and **nothing is drawn when the machine cannot answer.**
  --
  -- `topbar.lua` refused to draw indicators for subsystems that did not
  -- exist, on the grounds that a picture which lies about what the system
  -- knows is worse than a gap. What decides it now is the audio server
  -- rather than this file: on a board with no sound card it answers that
  -- there is none, and `audio.stats()` says so. The rule is the same one;
  -- it is answered instead of remembered. (This said `needs audio` decided
  -- it, which it never did - `/Devices/audio` reaches every program.)
  --
  --
  -- **Line glyphs, in the bar's own words' colour** (`roadmap.md` 6zl,
  -- `docs/statusicons.html`, agreed 27 September): Lucide's, 19 pixels in
  -- the 32-pixel bar, where Haiku's coloured pictures were 24. A muted
  -- speaker is crossed, which the picture could not say.
  --
  local ly = (self.h - LINE) // 2
  local level, muted = heard.level, heard.muted

  if level then
    x = x - PAD - LINE
    g:line_icon(x, ly, muted and "muted" or "sound", theme.tab_text, LINE)
    self.volume_x = x
  else
    self.volume_x = nil
  end

  --
  -- **The network is always said, offline too** - Diego's 3: the picture
  -- used to be left out with no card, and a gap says nothing where crossed
  -- arcs say the machine is not on a network. The Ethernet port for a card,
  -- since every card Kosmos drives is wired; Wi-Fi's arcs when there is a
  -- driver for one.
  --
  local net = heard.network

  if net then
    x = x - PAD - LINE
    g:line_icon(x, ly, net, theme.tab_text, LINE)
    self.network_x = x

    -- Said in the log when it changes, as the battery is, for a harness.
    if self.network_said ~= net then
      print("deskbar: network " .. net)
      self.network_said = net
    end
  else
    self.network_x = nil
  end

  --
  -- The battery: its charge, and "charging" while it is.
  --
  -- **Diego, 19 September: "The battery indicator is a must", "As I now
  -- don't know what battery is left".** It used to be the picture and a
  -- question mark, drawn before anything could read one, so the reading
  -- would replace the query and nothing else would move - and that is what
  -- it does. The ThinkPad's embedded controller is read by the kernel every
  -- thirty seconds (`hal/pc/ec.c`) and `/Devices/battery` is that reading.
  --
  -- **Nothing is drawn where there is nothing to read**, by the volume's
  -- rule above: a charge on a machine that cannot measure one would be
  -- indistinguishable from one that works. At ten per cent and falling, or
  -- when the controller calls it critical, the number turns red.
  --
  -- Said in the log when the words change, which is how a harness knows
  -- what the bar is showing without reading pixels as text.
  --
  local bat = heard.battery

  if bat then
    --
    -- The glyph says how full, or the bolt that it is charging - which the
    -- word "charging" used to (Diego's 2) - and the number beside it is
    -- what a person looks for. Low, glyph and number both in the look's red.
    --
    local charging = bat.state == "charging"
    local label = ("%d%%"):format(bat.percent)
    local low = bat.critical == 1
                or (bat.state == "discharging" and bat.percent <= 10)
    local ink = low and (theme.bad or 0xffe04848) or theme.tab_text
    local glyph = battery_glyph(bat)
    local lw = gfx.measure(label)

    x = x - PAD - LINE - 4 - lw
    self.battery_x = x

    g:line_icon(x, ly, glyph, ink, LINE)
    g:text(x + LINE + 4, ty, label, ink)

    -- The log says the state in words, as it did, for whoever reads it.
    local said = label .. (charging and " charging" or "")

    if self.battery_said ~= said then
      print("deskbar: battery " .. said)
      self.battery_said = said
    end
  else
    self.battery_x = nil
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
  local busy = heard.busy
  local used, total = heard.used, heard.total

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
    -- No bevel, and no rounding (above `bar`). The shade says which one
    -- you are in; the bar's own colour, lighter or darker, says it is part
    -- of the bar.
    --
    if selected(w_) then
      g:fill(s.x, 2, s.w, self.h - 4, lit(face, PRESSED))
    elseif not w_.hidden then
      g:fill(s.x, 2, s.w, self.h - 4, face)
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
      -- Breathing while it starts; half there once it has failed.
      local fade = w_.failed and 128 or (w_.starting and breath()) or nil

      g:icon(s.x + 4, iy, w_.icon .. ".png", ICON, fade)
    end

    --
    -- The title, cut to what is left. `gfx.measure` rather than a character
    -- count, because the font is proportional and a count would cut "Web
    -- browser" and "MMMMMMMMMM" in the same place.
    --
    local room = s.w - ICON - 12
    local text = tostring(w_.title or "")
    local ink = theme.tab_text

    -- A starting name dimmed halfway to the button's face; a failure says
    -- so, in the colour this bar already uses for a battery running out.
    if w_.failed then
      text = text .. " did not start"
      ink = theme.bad or 0xffe04848
    elseif w_.starting then
      ink = halfway(theme.tab_text, face)
    end

    if room >= gfx.font.w then
      while #text > 1 and gfx.measure(text) > room do
        text = text:sub(1, #text - 1)
      end

      g:text(s.x + ICON + 8, ty, text, ink)
    end
  end
end

--------------------------------------------------------------------------
-- What a click on the bar does.
--------------------------------------------------------------------------

function bar:mouse(action, x, y)
  local _ = y

  if action ~= "press" then return false end

  if x < kosmos_w() then
    -- Lit by the repaint this press causes: returning true is what repaints,
    -- and by then the menu is open and `#win.menus` says so. See "Instant
    -- feedback" in `ui.md` §16.13.
    open_kosmos_menu()
    return true
  end

  if self.volume_x and x >= self.volume_x - 4 and x < self.volume_x + LINE + 4 then
    fs.send("/Running/wm", { type = "launch", program = "/Kosmos/Apps/mixer.lua" })
    return true
  end

  if self.network_x and x >= self.network_x - 4 and x < self.network_x + LINE + 4 then
    fs.send("/Running/wm", { type = "launch", program = "/Kosmos/Apps/network.lua" })
    return true
  end

  if self.meters_x and x >= self.meters_x and x < self.meters_x + 26 then
    fs.send("/Running/wm", { type = "launch", program = "/Kosmos/Apps/sysmon.lua" })
    return true
  end

  if x >= right_x then
    -- The clock and the date open what has been said, as macOS's and
    -- Googlebook's do (`docs/notifications.html`); the date and time
    -- themselves are set in Preferences, Date & Time.
    open_history()
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

      -- Still starting: there is no window to raise, and it is on its way.
      if w_.starting then return true end

      local what = selected(w_) and "minimise" or "raise"

      --
      -- **The button changes on this press, not when somebody says so.**
      --
      -- It is the control you press when you cannot find a window, so it has
      -- to answer at once or you press it again. So the bar draws what it is
      -- about to ask for, out of what it already knows, and the repaint that
      -- returning true causes shows it. The `windows` event the request
      -- causes arrives after and finds nothing to change.
      --
      -- One answer is waited for first, the request's own, which is a pass
      -- of the window manager. Painting before asking would put the bar's
      -- frame ahead of the raise in the window manager's queue and bring the
      -- window up later by the same few milliseconds, and the window coming
      -- up is the larger part of what a person is watching. Measured under
      -- QEMU with `trace` on: about a tenth of a second from the window
      -- manager moving the focus to the bar's finished frame, on this path
      -- and on a window's tab alike.
      --
      -- Not a second memory of the focus: the window manager's list still
      -- wins. A refused request - the window closed in between - asks for
      -- the list again at once, so the guess does not stick.
      --
      for _, other in ipairs(running) do other.focused = nil end

      if what == "raise" then
        w_.focused = true
        w_.hidden = nil
      else
        w_.hidden = true
      end

      local ok = fs.send("/Running/wm", { type = what, window = w_.handle })

      if not ok then
        refresh()
      end

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
  if x < kosmos_w() then
    kosmos_context_menu()
    return true
  end

  return false
end

--------------------------------------------------------------------------
-- **The dock** (`roadmap.md`, a dock at the bottom; `docs/dock.html`): the
-- same bar's Kosmos button and what runs, and what is pinned, drawn as
-- Googlebook's dock - an application a cell, its picture, a mark under it
-- while it runs - and a strip across the top for the time and the
-- indicators. What is where is `dock.lua`'s arithmetic; this draws it.
--------------------------------------------------------------------------

local dock_frame = nil        -- the dock's own work each pass, below
local dock_tip_hide = nil     -- the name over an icon, taken away

if DOCKED then
  local wmproto = use("/Kosmos/Libraries/wmproto.lua")
  local saved = fs.read("/Home/Preferences/dock")
  local pins = (type(saved) == "table" and type(saved.pins) == "table")
               and saved.pins or dock.PINS
  local items = {}
  local wanted_w = nil            -- the dock's width once its cells are known
  local drag = nil                -- an icon pressed and maybe dragged (`bar:mouse`)
  local act, save_pins, show_tip_text

  local function pinned_icon(name)
    local attrs = programs[name]

    return attrs and attrs.icon or nil
  end

  -- The cells and their places, from what runs now; and the width the dock
  -- should be, which `on_frame` asks for outside the draw.
  local function lay_out()
    items = dock.items(pins, running, pinned_icon)

    local width = dock.layout(items, gfx.measure("Kosmos"))

    -- Not while an icon is dragged: the dock would grow or shrink and move
    -- under the pointer.
    wanted_w = (FLOATING and not (drag and drag.at)) and width or nil
  end

  --
  -- **The pins, written where they are read** - `/Home/Preferences/dock`,
  -- beside whatever else is kept there - and the dock laid out again.
  --
  function save_pins(new, what, name)
    pins = new

    local file = fs.read("/Home/Preferences/dock")

    if type(file) ~= "table" then file = {} end

    file.pins = new

    if not fs.getattr("/Home/Preferences") then
      fs.send("/Home/Preferences", { type = "mkdir" })
    end

    local ok, why = fs.write("/Home/Preferences/dock", file)

    print(("deskbar: %s %s - the dock is %s%s"):format(what, name, table.concat(new, ","),
          ok and "" or (", not kept: " .. tostring(why))))
    lay_out()
    win.dirty = true
  end

  -- A colour at an opacity: the dock's surface is the look's window, a
  -- little see-through, as the drawing's is.
  local function at(c, a) return (c & 0x00ffffff) | (a << 24) end

  function bar:draw(g)
    lay_out()

    --
    -- **While an icon is dragged**, the cells as they would be if it were
    -- let go here (`dock.drop`): the others making room, the dragged one
    -- drawn faded under the pointer - or gone, held well above the dock,
    -- where letting go takes it out. The real cells stay `items`: the drop
    -- is worked out against them.
    --
    local shown, dragged = items, nil

    if drag and drag.at then
      local preview = dock.drop(pins, items, drag.name, drag.at.x, drag.at.y)

      shown = dock.items(preview, running, pinned_icon)
      dock.layout(shown, gfx.measure("Kosmos"))
      dragged = drag.name
    end

    g:fill(0, 0, self.w, self.h, 0x00000000)

    -- A hairline of white at a twelfth round the surface, as the drawing
    -- has (`--d-line`): what tells a dark dock from a dark window under it.
    -- The fill replaces rather than blends, so the inner shape leaves a
    -- ring of the outer one.
    if FLOATING then
      g:fill_round(0, 0, self.w, self.h, 0x14ffffff, dock.RADIUS)
      g:fill_round(1, 1, self.w - 2, self.h - 2, at(theme.window, 0xd8), dock.RADIUS - 1)
    else
      g:fill(0, 0, self.w, self.h, at(theme.window, 0xd8))
      g:fill(0, 0, self.w, 1, 0x14ffffff)
    end

    -- Centred in the whole width, as the floating dock is on the screen.
    local off = FLOATING and 0 or (self.w - (dock.layout(items, gfx.measure("Kosmos")))) // 2
    local cy = self.h // 2

    for _, it in ipairs(shown) do
      local x = it.x + off

      if it.kind == "app" and it.name == dragged then
        goto next_cell                    -- drawn under the pointer, below
      end

      if it.kind == "kosmos" then
        local open = #win.menus > 0 or launcher_open
                     or (launcher_asked ~= nil
                         and sys.ticks() - launcher_asked < 2 * counter_hz)
        local bh = dock.KOSMOS_H

        g:fill_round(x, cy - bh // 2, it.w, bh, open and theme.accent or theme.raised, bh // 2)
        g:icon(x + dock.KOSMOS_IN, cy - dock.MARK // 2, "App_Deskbar.png", dock.MARK)
        g:text(x + dock.KOSMOS_IN + dock.MARK + 8, cy - gfx.font.h // 2, "Kosmos",
               open and theme.text_on or theme.text)
      elseif it.kind == "separator" then
        g:fill(x + it.w // 2, cy - 15, 1, 30, at(theme.text_dim, 0x70))
      else
        g:icon(x + (it.w - dock.ICON) // 2, cy - dock.ICON // 2 - 3, it.icon .. ".png", dock.ICON)

        if it.running then
          local mw = it.front and 16 or 6

          g:fill_round(x + (it.w - mw) // 2, self.h - 9, mw, 4,
                       it.front and theme.accent or theme.text_dim, 2)
        end
      end

      ::next_cell::
    end

    if dragged and drag.at.y >= -dock.REMOVE_ABOVE then
      g:icon(drag.at.x + off - dock.ICON // 2, cy - dock.ICON // 2 - 3,
             drag.icon .. ".png", dock.ICON, 150)
    end

    self.offset = off
    kosmos_at = items[1] and items[1].x + off or nil
  end

  function dock_open_launcher()
    -- Open already - the Windows key a second time - and so closed: a press
    -- on the button never gets here then, since the window manager closes a
    -- popup on a press outside it and goes no further.
    if launcher_open and launcher_handle then
      fs.send("/Running/wm", { type = "close", window = launcher_handle })
      print("deskbar: the launcher closed")
      return
    end

    launcher_asked = sys.ticks()
    fs.send("/Running/wm", { type = "launch", program = "launchpad" })
    print("deskbar: the launcher asked for")
    win.dirty = true
  end

  --
  -- **A press on an icon is held until it is let go** (`roadmap.md`, the
  -- dock arranged by hand): moved more than a few pixels it is a drag, and
  -- let go it is dropped (`dock.drop`) - moved, kept, taken out, let go of;
  -- let go where it was pressed it is a click, and does what a click does.
  -- So a click acts on the release, which is what lets a press become a
  -- drag without opening what it pressed on.
  --
  local DRAG_FROM = 6

  function bar:mouse(action, x, y)
    if action == "move" and drag then
      if not drag.at and math.abs(x - drag.px) <= DRAG_FROM and math.abs(y - drag.py) <= DRAG_FROM then
        return false
      end

      if not drag.at then print("deskbar: dragging " .. drag.name) end

      drag.at = { x = x - (self.offset or 0), y = y }

      -- Held above the dock: what letting go there does, said over it.
      if drag.at.y < -dock.REMOVE_ABOVE and drag.pinned then
        show_tip_text("Remove from Dock", win.origin_x + x)
      elseif dock_tip_hide then
        dock_tip_hide()
      end

      return true
    end

    if action == "release" and drag then
      local d = drag

      drag = nil

      if dock_tip_hide then dock_tip_hide() end

      if d.at then
        local new, what = dock.drop(pins, items, d.name, d.at.x, d.at.y)

        if what then save_pins(new, what, d.name) end

        return true
      end

      act(d.item)
      return true
    end

    if action ~= "press" then return false end

    if dock_tip_hide then dock_tip_hide() end

    local it = dock.hit(items, x - (self.offset or 0))

    if not it then return false end

    --
    -- **The Kosmos button opens the launcher** - every application, as a
    -- grid with its search, above the dock (`launchpad`, which reads where
    -- from `anchor` below). A press while it is open never arrives here:
    -- the window manager closes a popup on a press outside it and stops
    -- there, so a second press closes it. The menu it used to open is the
    -- right button's (`on_context`).
    --
    if it.kind == "kosmos" then
      dock_open_launcher()
      return true
    end

    drag = { item = it, name = it.name, icon = it.icon, pinned = it.pinned,
             px = x, py = y }
    return true
  end

  -- What a click on an application's icon does (`dock.action`).
  function act(it)
    local what, arg = dock.action(it)

    if what == "launch" then
      fs.send("/Running/wm", { type = "launch", program = arg })
      print("deskbar: the dock launched " .. arg)
    elseif what == "raise" or what == "minimise" then
      -- Drawn on this press from what is known, as the bar's buttons are.
      for _, other in ipairs(running) do
        if other.handle == arg then
          other.focused = (what == "raise") or nil
          other.hidden = (what == "minimise") or nil
        elseif what == "raise" then
          other.focused = nil
        end
      end

      if not fs.send("/Running/wm", { type = what, window = arg }) then refresh() end
    end

    return true
  end

  -- The right button on the Kosmos button: the Deskbar's menu, with
  -- Restart and Shut Down, until quick settings hold those (step 5).
  function bar:on_context(x, _)
    local it = dock.hit(items, x - (self.offset or 0))

    if it and it.kind == "kosmos" then
      open_kosmos_menu()
      return true
    end

    --
    -- **An application's own menu**, as macOS's dock has: Open, or Show
    -- when it runs; Keep in Dock or Remove from Dock; Quit when it runs.
    --
    if it and it.kind == "app" then
      local rows = {}

      rows[#rows + 1] = it.running
        and { text = "Show", on_choose = function()
                local what, arg = dock.action(it)

                if what == "minimise" then what = "raise" end
                if what == "raise" then fs.send("/Running/wm", { type = "raise", window = arg }) end
              end }
        or { text = "Open", on_choose = function()
               fs.send("/Running/wm", { type = "launch", program = it.name })
             end }

      rows[#rows + 1] = it.pinned
        and { text = "Remove from Dock", on_choose = function()
                save_pins(dock.unpin(pins, it.name), "removed", it.name)
              end }
        or { text = "Keep in Dock", on_choose = function()
               save_pins(dock.pin(pins, it.name), "kept", it.name)
             end }

      if it.running then
        rows[#rows + 1] = { text = "Quit", on_choose = function()
          for _, w_ in ipairs(it.windows or {}) do
            if w_.handle then fs.send("/Running/wm", { type = "close", window = w_.handle }) end
          end
        end }
      end

      win:open_menu(win.origin_x + (self.offset or 0) + it.x, win.origin_y - 6, rows, true)
      print(("deskbar: the menu of %s, %d rows"):format(it.name, #rows))
      return true
    end

    return false
  end

  --
  -- **Where the launcher goes**: the middle of the dock's top edge, on the
  -- screen, which `launchpad` centres its grid over - the Kosmos button,
  -- Super and Space, and anything else that opens it alike.
  --
  win:publish("anchor", function()
    return ("%d,%d"):format(win.origin_x + win.w // 2, win.origin_y)
  end)

  --
  -- **A name over the icon under the pointer** (Diego, 3 October: "hovering
  -- over the icons in the dock app icons should tell the name of the app",
  -- and a picture of macOS's): a dark pill with the name in white, centred
  -- over the icon, a small arrow down to it - a *tip* window, which the
  -- window manager shows and nothing else. The application's own name
  -- (`kosmos: name`), not its window's title. The window manager tells the
  -- dock where the pointer is while it is over it, and -1 when it leaves.
  --
  local tip, tip_for = nil, nil
  local TIP_H, TIP_ARROW, TIP_IN = 26, 6, 12
  local TIP_BACK, TIP_INK = 0xf0202227, 0xffffffff

  local function tip_name(it)
    local attrs = programs[it.name]

    if attrs and attrs.title and attrs.title ~= "" then return attrs.title end

    local name = tostring(it.title or it.name)

    return name:sub(1, 1):upper() .. name:sub(2)
  end

  local function hide_tip()
    if tip then
      tip:close()
      tip, tip_for = nil, nil
    end
  end

  local show_tip_at

  local function show_tip(it)
    if tip and tip_for == it then return end

    show_tip_at(tip_name(it), win.origin_x + (bar.offset or 0) + it.x + it.w // 2, it)
  end

  -- Any words over the dock, centred on `centre` - "Remove from Dock" over
  -- an icon held above it.
  function show_tip_text(text, centre)
    if tip and tip_for == text then return end

    show_tip_at(text, centre, text)
  end

  function show_tip_at(name, centre, what)
    hide_tip()

    local w = gfx.measure(name) + 2 * TIP_IN
    local h = TIP_H + TIP_ARROW

    tip = ui.window{ title = "Deskbar tip", w = w, h = h,
                     x = centre - w // 2, y = win.origin_y - h - 4, tip = true }

    if not tip then return end

    tip_for = what

    local face = ui.view{ x = 0, y = 0, w = w, h = h }

    function face:draw(g)
      g:fill(0, 0, self.w, self.h, 0x00000000)
      -- A hairline round it, as the dock has, for a dark window behind.
      g:fill_round(0, 0, self.w, TIP_H, 0x30ffffff, TIP_H // 2)
      g:fill_round(1, 1, self.w - 2, TIP_H - 2, TIP_BACK, TIP_H // 2 - 1)
      g:triangle(self.w // 2 - TIP_ARROW, TIP_H, self.w // 2 + TIP_ARROW, TIP_H,
                 self.w // 2, TIP_H + TIP_ARROW, TIP_BACK)
      g:text(TIP_IN, (TIP_H - gfx.font.h) // 2, name, TIP_INK, TIP_BACK)
    end

    tip:add(face)
    tip:paint()
    print("deskbar: tip " .. name)
  end

  win.on_hover = function(_, x, _)
    local it = (x >= 0) and dock.hit(items, x - (bar.offset or 0)) or nil

    if it and it.kind == "app" then
      show_tip(it)
    else
      hide_tip()
    end

    return false
  end

  wmproto.track(win.handle, true)
  dock_tip_hide = hide_tip

  --
  -- **What is pinned, and where each cell is**, to read - and `pin` to add
  -- one at the end: the launcher grid's Add to Dock, and `setprop
  -- /Running/Deskbar/pin music` by hand.
  --
  win:publish("pins", function() return table.concat(pins, ",") end)

  win:publish("pin", function() return "" end, function(v)
    local name = tostring(v)

    if name ~= "" then save_pins(dock.pin(pins, name), "kept", name) end
  end)

  win:publish("cells", function()
    local out = {}

    for _, it in ipairs(items) do
      out[#out + 1] = ("%s %d %d"):format(it.kind == "app" and it.name or it.kind,
                                          it.x + (bar.offset or 0), it.w)
    end

    return table.concat(out, "; ")
  end)

  --
  -- **The strip across the top**: the time and the date at the left, the
  -- indicators at the right, in the desktop's own words' colour over the
  -- wallpaper - a shadow under each so they read on a light picture too.
  --
  local strip = ui.view{ x = 0, y = 0, w = topstrip and topstrip.w or W, h = dock.STRIP_H }

  local function shadowed(g, x, y, text, ink)
    g:text(x + 1, y + 1, text, 0x80000000)
    g:text(x, y, text, ink)
  end

  function strip:draw(g)
    g:fill(0, 0, self.w, self.h, 0x00000000)

    local ink = theme.desktop_text or 0xffffffff
    local ty = (self.h - gfx.font.h) // 2
    local ly = (self.h - LINE) // 2
    local time = heard.now and clock.time_string(heard.now) or ""
    local date = heard.now and clock.date_string(heard.now) or ""

    shadowed(g, 18, ty, time, ink)
    shadowed(g, 18 + gfx.measure(time) + 10, ty, date, ink)
    self.clock_w = 18 + gfx.measure(time) + 10 + gfx.measure(date)

    -- Something said since the history was last opened.
    if news() then
      g:fill_round(self.clock_w + 8, (self.h - 6) // 2, 6, 6, theme.accent, 3)
    end

    local x = self.w - 18

    self.volume_x, self.network_x, self.battery_x = nil, nil, nil

    local bat = heard.battery

    if bat then
      local label = ("%d%%"):format(bat.percent)

      x = x - gfx.measure(label)
      shadowed(g, x, ty, label, ink)
      x = x - 4 - LINE
      g:line_icon(x, ly, battery_glyph(bat), ink, LINE)
      self.battery_x = x
      x = x - 14
    end

    if heard.level then
      x = x - LINE
      g:line_icon(x, ly, heard.muted and "muted" or "sound", ink, LINE)
      self.volume_x = x
      x = x - 14
    end

    if heard.network then
      x = x - LINE
      g:line_icon(x, ly, heard.network, ink, LINE)
      self.network_x = x
    end
  end

  -- What a press on the strip opens, as the bar's indicators do.
  local function strip_press(x)
    local function near(at_) return at_ and x >= at_ - 6 and x < at_ + LINE + 6 end
    local program = nil

    if near(strip.volume_x) then
      program = "/Kosmos/Apps/mixer.lua"
    elseif near(strip.network_x) then
      program = "/Kosmos/Apps/network.lua"
    elseif x < (strip.clock_w or 0) + 16 then
      open_history()
      return
    end

    if program then fs.send("/Running/wm", { type = "launch", program = program }) end
  end

  if topstrip then
    topstrip:add(strip)
    topstrip:paint()
  end

  --
  -- **The strip is served from the dock's loop** - one process, two windows,
  -- and the kit's loop polls one: its presses asked for without waiting, and
  -- it is painted again when what it shows has changed. And the dock asks
  -- for the width its cells want, outside its own draw.
  --
  local strip_said = nil

  function dock_frame(self)
    if tip then wmproto.poll(tip.handle, 0) end

    if topstrip then
      local reply = wmproto.poll(topstrip.handle, 0)

      for _, ev in ipairs(reply and reply.events or {}) do
        if ev.type == "mouse" and ev.action == "press" then strip_press(ev.x) end
      end

      local now = table.concat({ heard.now and clock.time_string(heard.now) or "",
                                 tostring(news()),
                                 tostring(heard.level), tostring(heard.muted),
                                 tostring(heard.network),
                                 heard.battery and heard.battery.percent or "",
                                 tostring(theme.desktop_text) }, "|")

      if now ~= strip_said then
        strip_said = now
        topstrip:paint()
      end
    end

    if wanted_w and wanted_w ~= self.w then
      local w = wanted_w

      wanted_w = nil
      self:resize(w, H)
      return true
    end

    return false
  end
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
if not tostring(args or ""):match("%-%-again") then
  -- The list, or what a machine nobody has told opens. `/Kosmos/Libraries/startup.lua`
  -- holds both so that this and the panel cannot disagree about it.
  local items = use("/Kosmos/Libraries/startup.lua").items()

  local started = 0

  for _, name in ipairs(items) do
    name = tostring(name)

    -- Only what this Deskbar would list. A name in the file that is no
    -- longer in `/bin` is a stale tick, not a reason to send the window
    -- manager a program it cannot find.
    local known = programs[name] ~= nil

    -- Narrated, because this is the step nobody could see. Four windows
    -- opening at login is four `launch` messages from here, and when one of
    -- them does not arrive there is nothing on the screen to say which - so
    -- each is announced with what came back. `log deskbar` at the prompt
    -- reads them after the desktop has been left.
    if not known then
      print(("deskbar: %s is not in /bin, so it was not started"):format(name))
    else
      local sent = fs.send("/Running/wm", { type = "launch", program = name })

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

-- Repainted every pass while a program breathes on the bar - the passes
-- themselves come twelve times a second then (`pace_breathing`) - and on
-- its own reasons otherwise.
--
-- And the dock's own work (`dock_frame`, above), and a new place for the bar
-- once the write that asked for it has been answered.
--
win.on_frame = function(self)
  if leaving then
    local to = leaving

    leaving = nil
    again(to[1], to[2])
    return false
  end

  local more = dock_frame and dock_frame(self) or false

  return any_starting() or more
end

-- The first second at once, after the first picture (`heard`, above): a
-- pass a tick from now rather than a second, which `pace_breathing` puts
-- back when the tick has listened.
win.poll_wait_ticks = 1

win:run()
