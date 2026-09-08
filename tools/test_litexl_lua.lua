-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- Does Lite XL's Lua still load? Answered on this machine, in a second.
--
-- **The port's Lua half is nineteen thousand lines of somebody else's
-- code**, and until this existed the only way to find out whether it still
-- loaded was to finish the whole port and boot a machine. It does not need
-- one: the editor is Lua 5.4 and so is `build/host/lua`, so with the six C
-- modules stubbed it can be loaded, initialised and asked what it wanted.
--
-- That is how the shape of the C half was decided rather than guessed. Two
-- things came out of the first run and neither was in the plan:
--
--   * `luaL_requiref(L, name, fn, 1)` sets each module as a **global**, and
--     `start.lua` relies on it - it says `system.get_file_info` with no
--     `require` in sight.
--
--   * **`dirmonitor` cannot refuse.** It was written to raise, because
--     Kosmos cannot watch a directory; `core.init()` then died at
--     `core/dirwatch.lua:41`, because the editor makes a monitor at startup
--     and indexes it whether or not anything is watched. A module that
--     refuses is only honest when nobody needs it to exist.
--
-- What this does *not* check is that the editor works - there is no window,
-- no font and no event here. It checks that the module graph resolves and
-- that `core.init()` returns, which is the part that breaks silently when
-- somebody upgrades the vendored tree.

-- Can Lite XL's Lua load at all? Answered on the build machine, with the
-- C modules stubbed, before any Kosmos build machinery is written for it.
local DATA = "runtime/upstream/lite-xl/data"
package.path = DATA .. "/?.lua;" .. DATA .. "/?/init.lua;" .. package.path

-- `luaL_requiref(L, name, fn, 1)` sets the module as a *global* as well as
-- in package.loaded, and lite-xl's Lua relies on that: `start.lua` says
-- `system.get_file_info` with no require in sight.
local function stub(name, t)
  package.loaded[name] = t
  _G[name] = t
end

-- Every call is recorded, so what comes out of this run is the list of
-- host functions the editor's *startup* actually needs - which is the
-- thing worth knowing before writing them.
local used = {}
local sys_impl = {
  get_time      = function() return os.clock() end,
  sleep         = function() end,
  get_file_info = function(p)
    local f = io.open(p, "r")
    if f then f:close() return { type = "file", size = 0, modified = 0 } end
    return nil
  end,
  list_dir      = function() return {} end,
  absolute_path = function(p) return p end,
  get_fs_type   = function() return "ext4" end,
  get_process_id= function() return 1 end,
  window_has_focus = function() return true end,
  get_window_size  = function() return 800, 600, 0, 0 end,
  get_window_mode  = function() return "normal" end,
  poll_event    = function() return nil end,
  wait_event    = function() return false end,
  fuzzy_match   = function() return nil end,
  path_compare  = function(a, _, b, _) return a < b end,
  set_window_title = function() end,
  set_window_mode  = function() end,
  set_text_input_rect = function() end,
  set_cursor    = function() end,
  chdir         = function() end,
  mkdir         = function() return true end,
  rmdir         = function() return true end,
  get_clipboard = function() return "" end,
  set_clipboard = function() end,
  raise_window  = function() end,
  show_fatal_error = function(a, b) print("FATAL", a, b) end,
}
stub("system", setmetatable({}, {
  __index = function(_, k)
    used[k] = (used[k] or 0) + 1
    local f = sys_impl[k]
    if f then return f end
    return function() return nil end
  end }))
local rused = {}
local fontmt = {}
fontmt.__index = {
  set_size = function() end, get_size = function() return 14 end,
  get_width = function(_, s) return #tostring(s) * 8 end,
  get_height = function() return 16 end,
  subpixel_scale = function() return 1 end,
  copy = function(self) return setmetatable({}, fontmt) end,
  set_tab_size = function() end,
  get_path = function() return "font.ttf" end,
}
stub("renderer", setmetatable({
  font = { load = function() return setmetatable({}, fontmt) end,
           group = function(t) return t[1] end },
  draw_rect = function() end,
  draw_text = function(_, _, x) return x end,
  set_clip_rect = function() end,
  begin_frame = function() end,
  end_frame = function() end,
  get_size = function() return 800, 600 end,
}, { __index = function(_, k)
  rused[k] = true
  return function() return 0 end
end }))
stub("regex", { compile = function() return nil end, ANCHORED = 0 })
stub("process", {})
-- Upstream's own `dummy.c` shape: "single" mode, -1 from watch, nothing
-- from check. The editor then rescans instead of being told.
stub("dirmonitor", { new = function()
  return {
    mode    = function() return "single" end,
    watch   = function() return -1 end,
    unwatch = function() return -1 end,
    check   = function() return false end,
  }
end })
stub("utf8extra", { match = string.match, len = function(s) return #s end,
                    char = string.char, byte = string.byte,
                    sub = string.sub, codepoint = string.byte })

ARGS = { "lite-xl" }
PLATFORM = "Kosmos"
ARCH = "aarch64-kosmos"
EXEFILE = DATA:gsub("/data$", "") .. "/lite-xl"
HOME = "/home"
SCALE = 1

local ok, err = pcall(function()
  dofile(DATA .. "/core/start.lua")
end)
if not ok then print("FAIL: start.lua did not run: " .. tostring(err)) os.exit(1) end

local ok2, err2 = pcall(function() return require("core") end)
if not ok2 then print("FAIL: require 'core' failed: " .. tostring(err2)) os.exit(1) end

local ok3, err3 = pcall(function()
  local core = require("core")
  core.init()
end)
if not ok3 then print("FAIL: core.init() failed: " .. tostring(err3)) os.exit(1) end



local wanted = {
  "absolute_path", "chdir", "get_file_info", "get_time", "list_dir", "mkdir",
}

local missing = {}
for _, name in ipairs(wanted) do
  if not used[name] then missing[#missing + 1] = name end
end

if #missing > 0 then
  print("FAIL: startup no longer asks for: " .. table.concat(missing, " "))
  os.exit(1)
end

print(("PASS: %d checks on Lite XL's Lua loading, on this machine.")
      :format(3 + #wanted))
