-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Lite XL, a real editor, on Kosmos.
-- kosmos: application
-- kosmos: icon App_Pe
-- kosmos: section applications
--
--   wm litexl                     the editor, with /home as its project
--   wm litexl:/home/notes.txt     with that file open
--
-- Only in an image built with `make LITEXL=1`. `docs/litexl.md` is the
-- account of the port; this is its Lua half - the part `main.c` would have
-- been on a hosted system, and it is Lua here for the reason everything else
-- about this port ended up Lua: what the editor needs is a window, a
-- namespace and a clipboard, and on Kosmos all three are conversations with
-- servers.
--
-- **The editor keeps its own loop.** `core.run()` is Lite XL's, unchanged,
-- because the scheduler for its background work - the cursor blink,
-- highlighting, scanning a project - is a local inside `core/init.lua`, and a
-- caller stepping frames from outside would never run it. What Kosmos does
-- happens inside the two calls that loop makes when it waits,
-- `system.wait_event` and `system.sleep`: show the frame that was just drawn,
-- then ask the window manager what happened. A close from the desktop arrives
-- as Lite XL's own `quit`, so the window still closes.
--

local okkit, lx = pcall(use, "/kits/litexl")

if not okkit or type(lx) ~= "table" or not lx.swap_window then
  print("litexl: this image was not built with LITEXL=1")
  return
end

local ui = use("/lib/ui.lua")
local wmproto = use("/lib/wmproto.lua")

-- What this launcher decides without asking anybody - paths, the installed
-- tree, files, the event queue, key names, damage - is in a library of its
-- own, so `tools/test_litexl_host.lua` can check it without a machine.
local host = use("/lib/litexl_host.lua")

--
-- Two clocks, asked for rather than assumed.
--
-- The counter is for measuring and scheduler ticks are for waiting, and
-- neither rate is a constant: this said `sys.ticks() / 62500000`, which is
-- one board's counter, and on the other every second the editor measured
-- would have been a different length.
--
local counter_hz = (fs.read("/dev/cpu") or {}).counter_hz
local tick_hz = (sys.info() or {}).tick_hz

if not counter_hz or not tick_hz then
  print("litexl: the machine did not say how fast its clocks run")
  return
end

local function ticks_for(seconds)
  return math.max(0, math.ceil((seconds or 0) * tick_hz))
end

-- The longest this waits for the window manager in one go. A window that
-- stops answering is ended by the desktop, so even an idle editor comes
-- back and says so a few times a second.
local QUARTER = math.max(1, tick_hz // 4)

--------------------------------------------------------------------------
-- Modules.
--------------------------------------------------------------------------

local ROOT = "/lib/litexl/"

-- `require` over `use`'s namespace, which is the whole adapter: a module
-- name becomes candidate paths, and the namespace answers.
--
-- Seeded with the C modules the kit just registered. `api_load_libs` put
-- them in the *real* `package.loaded`, which is not this table - so without
-- this, `require "utf8extra"` goes looking for a Lua file that does not
-- exist and the C module sitting in a global right next to it is never
-- found.
local loaded = {}

for _, name in ipairs({ "system", "renderer", "regex", "process",
                        "dirmonitor", "utf8extra" }) do
  loaded[name] = _ENV[name]
end

-- `searchers` is empty rather than absent: `start.lua` inserts a loader into
-- it and would fail on nil, and nothing ever runs it because `require` below
-- never consults `package`.
package = { loaded = loaded, path = "", cpath = "", searchers = {},
            config = "/\n;\n?\n!\n-\n" }

function require(name)
  if loaded[name] ~= nil then return loaded[name] end
  local base = name:gsub("%.", "/")
  for _, cand in ipairs({ base .. ".lua", base .. "/init.lua" }) do
    local src = fs.read(ROOT .. cand)
    if type(src) == "string" then
      -- `_ENV`, not the default: a Kosmos program runs in a sandbox whose
      -- `__index` is `_G`, so a chunk loaded without it lands in the real
      -- globals and cannot see `require`, `system` or anything else here.
      local chunk, why = load(src, "@" .. cand, "t", _ENV)
      if not chunk then error(cand .. ": " .. tostring(why)) end
      local v = chunk(name, cand)
      if v == nil then v = true end
      loaded[name] = v
      return v
    end
  end
  error("module '" .. name .. "' not found")
end

--------------------------------------------------------------------------
-- Paths.
--
-- Lite XL thinks in a working directory and an installed tree; Kosmos has a
-- namespace and neither. The working directory is kept here, because it is
-- the editor's idea and no server has one. The installed tree is
-- `/lite-xl/data`, which is where `start.lua` puts `DATADIR` from the
-- `EXEFILE` below, and every path under it is read out of `/lib/litexl/`,
-- where the build put those same files.
--------------------------------------------------------------------------

local DATA = "/lite-xl/data"
local cwd = "/home"

local function absolute(p)
  return host.absolute(cwd, p)
end

-- The part of `full` under the installed tree, or nil when it is not there.
local function in_data(full)
  if full == DATA then return "" end
  return full:match("^/lite%-xl/data/(.+)$")
end

-- Where the namespace keeps a path, and whether that is the image.
local function ns_path(full)
  local sub = in_data(full)
  if sub then return ROOT .. sub, true end
  return full, false
end

-- The installed tree, worked out from the image's flat keys the first time
-- it is asked about. `litexl_host.lua` says why it has to be.
local data_tree

local function data()
  if not data_tree then
    data_tree = host.tree(fs.list("/lib") or {}, "litexl/")
  end

  return data_tree
end

local function data_info(sub) return data().info(sub) end
local function data_list(sub) return data().list(sub) end

local function file_info(path)
  local full = absolute(path)
  local sub = in_data(full)

  if sub then return data_info(sub) end

  local a, err = fs.getattr(full)
  if not a then return nil, err end

  return { type = (a.kind == "directory") and "dir" or "file",
           size = a.size or 0, modified = a.mtime or 0 }
end

--------------------------------------------------------------------------
-- `io`, `os`, `dofile` and `debug`, which this sandbox does not have.
--
-- Not oversights on Kosmos's part: each wants a global path tree, an
-- environment or a hosted C library, and none of those exists here. Lite XL
-- asks for them anyway - it loads and saves every document through
-- `io.open` - so it gets answers that are true, made of the namespace.
--------------------------------------------------------------------------

-- A file opened for reading is its whole text, read once, and one opened for
-- writing is stored whole when it is closed. `fs.write` already takes a value
-- larger than a message on every mount - in pieces to /ramfs, through a
-- region to the disk - so neither has to know which it is talking to.
local reading = host.reading

local function writing(full, initial)
  return host.writing(function(text)
    local ok, err = fs.write(full, text)
    if not ok then return nil, full .. ": " .. tostring(err) end
    return true
  end, initial)
end

io = {
  open = function(path, mode)
    mode = mode or "r"

    local full, image = ns_path(absolute(path))

    if mode:find("[wa+]") then
      if image then return nil, tostring(path) .. ": the editor's own files are read-only" end

      -- An append continues what is there, when what is there is text.
      local before = mode:find("a") and fs.read(full)

      return writing(full, type(before) == "string" and before or nil)
    end

    local text, err = fs.read(full)

    if type(text) ~= "string" then
      return nil, tostring(path) .. ": " .. tostring(err or "not a file")
    end

    return reading(text)
  end,

  lines = function(path, fmt)
    local f, err = io.open(path, "r")
    if not f then error(err, 2) end
    return f:lines(fmt)
  end,
}

-- A name to a server that can do something with it, and whether it did.
local function ask(full, message)
  local r, err = fs.send(full, message)
  if not r or r.ok == false then
    return false, tostring(err or (r and r.error) or "refused")
  end
  return true
end

os = {
  getenv  = function() return nil end,
  time    = function() return math.floor(sys.ticks() / counter_hz) end,
  clock   = function() return sys.ticks() / counter_hz end,
  date    = function() return "" end,
  exit    = function() end,
  remove  = function(p)
    local full, image = ns_path(absolute(p))
    if image then return nil, "read-only" end
    local ok, err = ask(full, { type = "delete" })
    if not ok then return nil, err end
    return true
  end,
  rename  = function(a, b)
    local from, image = ns_path(absolute(a))
    if image then return nil, "read-only" end
    local ok, err = ask(from, { type = "rename", to = absolute(b) })
    if not ok then return nil, err end
    return true
  end,
  tmpname = function() return "/home/.config/lite-xl/tmp" end,
}

-- `dofile`, over the namespace. Lite XL uses it for `start.lua` and for a
-- user config that may not exist; both are files here, just not on a tree.
function dofile(path)
  local full = ns_path(absolute(path))
  local src = fs.read(full)
  if type(src) ~= "string" then error("cannot open " .. tostring(path)) end
  return load(src, "@" .. tostring(path), "t", _ENV)()
end

-- `debug` is used for error reporting - a traceback when something throws,
-- and `getinfo` to name the file a plugin came from. Neither needs the real
-- library to be *useful*; both need it to exist.
debug = {
  traceback = function(msg) return tostring(msg or "") end,
  getinfo   = function() return { source = "@?", short_src = "?",
                                  currentline = 0, what = "Lua" } end,
}

--------------------------------------------------------------------------
-- The window.
--
-- `direct`, because the editor draws its own pixels: Lite XL's renderer
-- writes straight into the buffer the compositor blits, which is the reason
-- this port was worth doing. A direct window cannot be resized - its buffers
-- are allocated once, when it opens - so the size is chosen here, from the
-- screen, and stays.
--------------------------------------------------------------------------

local screen = fs.read("/dev/screen") or {}
local W = math.max(640, math.min(1100, (screen.width or 1024) - 80))
local H = math.max(400, math.min(760, (screen.height or 768) - 120))

local win, werr = ui.window{ title = "Lite XL", w = W, h = H, x = 40, y = 60,
                             direct = true }

if not win or not win:surface() then
  print("litexl: no window: " .. tostring(werr or "no shared surface"))
  return
end

lx.attach_window(win:surface())

--
-- Showing a frame.
--
-- Lite XL has drawn into the buffer the compositor is not showing and
-- recorded which rectangles changed. `commit` shows that buffer and hands
-- back the other one - which still holds the frame *before*, and Lite XL
-- only ever redraws what changed. So the rectangles just shown are copied
-- across before the editor is pointed at it, or the next frame would be
-- drawn over stale pixels and old text would show through wherever nothing
-- moved. A whole-window copy happens only when the whole window changed.
--
local function present()
  local damage = lx.take_damage()
  local x0, y0, x1, y1 = host.damage_bounds(damage, W, H)

  if not x0 then return end

  local shown = win:surface()

  if not win:commit{ x = x0, y = y0, w = x1 - x0, h = y1 - y0 } then
    error("litexl: the window manager went away")
  end

  local next_frame = win:surface()

  if damage == true then
    next_frame:blit(shown, 0, 0, W, H, 0, 0)
  else
    for i = 1, #damage, 4 do
      next_frame:blit(shown, damage[i], damage[i + 1], damage[i + 2],
                      damage[i + 3], damage[i], damage[i + 1])
    end
  end

  lx.swap_window(next_frame)
end

--------------------------------------------------------------------------
-- Events, in Lite XL's shape.
--------------------------------------------------------------------------

-- `wm litexl:--trace` says every event on its way in, which is how the
-- keyboard is debugged on a machine with no serial port but a log.
local TRACE = false

local events = host.queue(function(e)
  if TRACE then
    local parts = {}
    for i = 1, e.n do parts[i] = tostring(e[i]) end
    print("litexl: event " .. table.concat(parts, " "))
  end
end)

local push, pending = events.push, events.pending

-- Key names by keycode, in the names Lite XL binds strokes by.
local NAMES = host.KEY_NAMES

local held = {}
local decode = ui.key_decoder()

-- Text is what a key *meant*, and a key pressed with Control or Alt meant a
-- command rather than a character.
local function text(c)
  if host.is_text(c, held) then
    push("textinput", string.char(c))
  end
end

--
-- The pointer.
--
-- The window manager delivers the first button, and movement only while it
-- is held, so a press carries its own position and a double click is
-- counted here from how soon and how near the next one lands.
--
local mouse_x, mouse_y = 0, 0
local press_at, press_x, press_y, clicks = -math.huge, 0, 0, 0

local function pointer(ev)
  local dx, dy = ev.x - mouse_x, ev.y - mouse_y

  mouse_x, mouse_y = ev.x, ev.y

  if ev.action == "move" then
    push("mousemoved", ev.x, ev.y, dx, dy)
  elseif ev.action == "press" then
    local now = sys.ticks() / counter_hz

    if now - press_at < 0.4 and math.abs(ev.x - press_x) < 4
       and math.abs(ev.y - press_y) < 4 then
      clicks = clicks + 1
    else
      clicks = 1
    end

    press_at, press_x, press_y = now, ev.x, ev.y

    if dx ~= 0 or dy ~= 0 then push("mousemoved", ev.x, ev.y, dx, dy) end

    push("mousepressed", "left", ev.x, ev.y, clicks)
  elseif ev.action == "release" then
    push("mousereleased", "left", ev.x, ev.y)
  end
end

--
-- Select-all, copy, cut and paste, as the desktop decided them.
--
-- **Not the keys, the intents.** Control-A, C, X and V are taken by the
-- window manager before any window sees them, and what it sends on is what
-- it decided they *meant* - `{type = "copy"}` rather than a byte. So this
-- reads the same four intents every application here reads, and each
-- becomes the Lite XL command it names.
--
-- Which is why nothing had to change in this file when those keys moved off
-- the `Control-W` prefix: a window that reads intents does not know or care
-- which key produced one. That is the point of sending the intent.
--
local EDITS = {
  copy = "doc:copy", cut = "doc:cut", paste = "doc:paste",
  selectall = "doc:select-all",
}

local function pump(wait)
  local reply = wmproto.poll(win.handle, wait)

  if not reply then
    error("litexl: the window manager went away")
  end

  local events = reply.events or {}

  -- Transitions first, the order SDL gives a key and the text it typed: a
  -- press that runs a binding tells Lite XL to ignore the text behind it.
  for _, ev in ipairs(events) do
    if ev.type == "rawkey" then
      if TRACE then
        print(("litexl: rawkey %d %s"):format(ev.code, ev.down and "down" or "up"))
      end

      held[ev.code] = ev.down or nil

      local name = NAMES[ev.code]
      if name then push(ev.down and "keypressed" or "keyreleased", name) end
    end
  end

  for _, ev in ipairs(events) do
    if ev.type == "key" then
      if TRACE then print("litexl: key " .. tostring(ev.code)) end

      local a, b = decode(ev.code)
      text(a)
      text(b)
    elseif ev.type == "mouse" and not ev.menu then
      pointer(ev)
    elseif ev.type == "close" then
      push("quit")
    elseif EDITS[ev.type] then
      push("kosmos-edit", EDITS[ev.type])
    end
  end
end

--------------------------------------------------------------------------
-- The faces, handed over.
--
-- `ren_font_load` takes a filename and there is no `fopen`, so each face is
-- given to the renderer by name, out of the image: JetBrains Mono from
-- `assets/fonts/`, which the whole system draws from, and Lite XL's own UI
-- and icon faces from a table only a `LITEXL=1` image carries. Every one has
-- its licence beside it in the tree. Said either way, so the boot log
-- records where each face came from.
--------------------------------------------------------------------------

for _, want in ipairs({ "JetBrainsMono-Regular.ttf", "FiraSans-Regular.ttf",
                        "icons.ttf" }) do
  local how = lx.provide_image_font(want) and "from the image"
              or "MISSING from this image"

  print("litexl: font " .. want .. ": " .. how)
end

--------------------------------------------------------------------------
-- The host: what `system` forwards to.
--------------------------------------------------------------------------

lx.set_host{
  get_time = function() return sys.ticks() / counter_hz end,

  -- Both of the loop's waits: show what was drawn, then listen - for no
  -- longer than asked, and never for longer than a quarter of a second.
  sleep = function(s)
    present()
    pump(math.min(ticks_for(s), QUARTER))
  end,

  wait_event = function(s)
    present()
    if pending() > 0 then return true end
    pump(s and math.min(ticks_for(s), QUARTER) or QUARTER)
    return pending() > 0
  end,

  poll_event = function()
    if pending() == 0 then pump(0) end

    local e = events.pop()
    if not e then return nil end

    return table.unpack(e, 1, e.n)
  end,

  get_file_info = file_info,

  list_dir = function(p)
    local full = absolute(p)
    local sub = in_data(full)
    if sub then return data_list(sub) end
    return fs.list(full)
  end,

  absolute_path = absolute,

  chdir = function(p)
    local full = absolute(p)
    local info = file_info(full)

    if not info or info.type ~= "dir" then
      error("chdir: " .. full .. " is not a directory")
    end

    cwd = full
  end,

  mkdir = function(p)
    local full, image = ns_path(absolute(p))
    if image then return false, "read-only" end
    return ask(full, { type = "mkdir" })
  end,

  rmdir = function(p)
    local full, image = ns_path(absolute(p))
    if image then return false, "read-only" end
    return ask(full, { type = "delete" })
  end,

  get_fs_type         = function() return "kosmos" end,
  get_process_id      = function() return 1 end,

  get_window_size     = function() return W, H, 40, 60 end,
  set_window_size     = function() end,
  get_window_mode     = function() return "normal" end,
  set_window_mode     = function() end,
  set_window_bordered = function() end,
  set_window_hit_test = function() end,
  set_window_opacity  = function() return false end,
  window_has_focus    = function() return true end,
  raise_window        = function() end,
  set_cursor          = function() end,
  set_text_input_rect = function() end,
  clear_ime           = function() end,

  -- The title is Lite XL's - the document's name, and `*` while there are
  -- changes nobody saved - and the tab it goes in is the desktop's, so it
  -- is `retitle`: `win.title = ...` would change a copy. Lite XL asks only
  -- when the name changes, and on its first frame, which on a start with no
  -- file is "Lite XL" again. A name the window already has is not sent.
  set_window_title = function(title)
    if title ~= win.title then win:retitle(title) end
  end,

  show_fatal_error = function(title, message)
    print("litexl: " .. tostring(title) .. ": " .. tostring(message))
  end,

  get_clipboard = function() return wmproto.paste() or "" end,
  set_clipboard = function(t) wmproto.copy(t) end,
}

--------------------------------------------------------------------------
-- The editor.
--------------------------------------------------------------------------

ARGS, PLATFORM, ARCH = { "lite-xl" }, "Kosmos", "kosmos"
EXEFILE, HOME, SCALE = "/lite-xl/lite-xl", "/home", 1

-- Words starting `--` are this launcher's; the first other word is what to
-- open, a file or a project directory.
for word in (args or ""):gmatch("%S+") do
  if word == "--trace" then
    TRACE = true
  elseif not ARGS[2] then
    ARGS[2] = absolute(word)
  end
end

local src = fs.read(ROOT .. "core/start.lua")
local okstart, whystart = pcall(load(src, "@core/start.lua", "t", _ENV))

if not okstart then
  print("litexl: start.lua: " .. tostring(whystart))
  win:close()
  return
end

-- `start.lua` is the release exactly as upstream shipped it, and its version
-- is a placeholder their build fills in. This is the release that was
-- vendored, per `runtime/upstream/lite-xl/README.kosmos.md`.
VERSION = "2.1.7"

local okcore, core = pcall(require, "core")

if not okcore then
  print("litexl: require core: " .. tostring(core))
  win:close()
  return
end

-- The desktop's edits, turned into commands where Lite XL handles events.
do
  local command = require "core.command"
  local on_event = core.on_event

  function core.on_event(kind, a, ...)
    if kind == "kosmos-edit" then
      command.perform(a)
      return true
    end

    return on_event(kind, a, ...)
  end
end

-- `--trace` also says what Lite XL logged and which commands ran. An error a
-- command raises is caught by `core.try` and kept in the editor's own log
-- view, so without this it is invisible from outside the window.
if TRACE then
  for _, name in ipairs({ "log", "log_quiet", "warn", "error" }) do
    local original = core[name]

    if type(original) == "function" then
      core[name] = function(fmt, ...)
        local ok, said = pcall(string.format, tostring(fmt), ...)
        print("litexl: " .. name .. ": " .. (ok and said or tostring(fmt)))
        return original(fmt, ...)
      end
    end
  end

  local command = require "core.command"
  local perform = command.perform

  command.perform = function(name, ...)
    local done = perform(name, ...)
    print("litexl: command " .. tostring(name) .. " -> " .. tostring(done))
    return done
  end
end

local okinit, whyinit = pcall(core.init)

if not okinit then
  print("litexl: core.init: " .. tostring(whyinit))
  win:close()
  return
end

local okrun, whyrun = pcall(core.run)

if not okrun then
  print("litexl: " .. tostring(whyrun))
end

win:close()
