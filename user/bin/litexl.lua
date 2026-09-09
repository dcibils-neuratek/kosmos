-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Lite XL, a real editor, on Kosmos.
--
-- `docs/litexl.md` is the account of the port; this is its Lua half - the
-- part `main.c` would have been on a hosted system, and it is Lua here for
-- the reason everything else about this port ended up Lua: what the editor
-- needs is a window, a namespace and a clipboard, and on Kosmos all three
-- are conversations with servers.
--
-- **What works today: the editor initialises.** `start.lua` runs, the
-- 78-file module graph resolves, and `core.init()` returns. What is not
-- here yet is `core.run()` - the window, the event loop and the drawing -
-- and the fonts still come from a disk rather than the image.
--
local lx = use("/kits/litexl")
print("kit: ok, system=" .. tostring(system ~= nil)
      .. " renderer=" .. tostring(renderer ~= nil))

local ROOT = "/lib/litexl/"

-- `require` over `use`'s namespace, which is the whole adapter: a module
-- name becomes candidate paths, and the namespace answers.
--
-- Seeded with the C modules the kit just registered.
--
-- `api_load_libs` put them in the *real* `package.loaded`, which is not
-- this table - so without this, `require "utf8extra"` goes looking for a
-- Lua file that does not exist and the C module sitting in a global right
-- next to it is never found.
--
local loaded = {}

for _, name in ipairs({ "system", "renderer", "regex", "process",
                        "dirmonitor", "utf8extra" }) do
  loaded[name] = _ENV[name]
end
-- `searchers` is empty rather than absent: `start.lua` inserts a loader
-- into it and would fail on nil, and nothing ever runs it because
-- `require` below never consults `package`.
package = { loaded = loaded, path = "", cpath = "", searchers = {},
            config = "/\n;\n?\n!\n-\n" }

function require(name)
  if loaded[name] ~= nil then return loaded[name] end
  local base = name:gsub("%.", "/")
  for _, cand in ipairs({ base .. ".lua", base .. "/init.lua" }) do
    local src = fs.read(ROOT .. cand)
    if src then
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

-- The host: what `system` forwards to. Only what startup asks for.
lx.set_host{
  get_time      = function() return sys.ticks() / 62500000 end,
  sleep         = function(s) sys.sleep(math.floor((s or 0) * 250)) end,
  get_file_info = function(p)
    local a = fs.getattr and fs.getattr(p)
    if a then return { type = a.kind == "directory" and "dir" or "file",
                       size = a.size or 0, modified = 0 } end
    return nil
  end,
  list_dir      = function(p) return fs.list and fs.list(p) or {} end,
  absolute_path = function(p) return p end,
  mkdir         = function() return true end,
  chdir         = function() return true end,
  get_fs_type   = function() return "kosmos" end,
  set_window_bordered = function() end,
  set_window_hit_test = function() end,
  window_has_focus = function() return true end,
  get_window_size  = function() return 800, 600, 0, 0 end,
  poll_event    = function() return nil end,
  set_window_title = function() end,
  set_cursor    = function() end,
  get_clipboard = function() return "" end,
  set_clipboard = function() end,
}

--
-- `os`, which this sandbox does not have.
--
-- Not an oversight on Kosmos's part: `os.getenv` wants an environment,
-- `os.time` a clock and `os.remove` a global path tree, and none of the
-- three exists here. Lite XL asks for them anyway, so it gets the answers
-- that are true - no variables set, the counter for a clock, and a
-- filesystem reached through the namespace.
--
os = {
  getenv = function() return nil end,
  time   = function() return math.floor(sys.ticks() / 62500000) end,
  clock  = function() return sys.ticks() / 62500000 end,
  date   = function() return "" end,
  exit   = function() end,
  remove = function() return nil, "read-only" end,
  rename = function() return nil, "read-only" end,
  tmpname = function() return "/tmp/litexl" end,
}

--
-- The faces, handed over as bytes.
--
-- `ren_font_load` takes a filename and there is no `fopen`: a path means
-- nothing without a namespace, so the Lua side reads and the C side looks
-- the name up. `doom_kosmos.c` met the same wall with the WAD.
--
for _, f in ipairs({ "JetBrainsMono-Regular.ttf", "FiraSans-Regular.ttf",
                     "icons.ttf" }) do
  local bytes = fs.read("/home/fonts/" .. f)
  if bytes then lx.provide_font(f, bytes) end
  print("font " .. f .. ": " .. (bytes and (#bytes .. " bytes") or "MISSING"))
end

--
-- `dofile`, over the namespace. Lite XL uses it for `start.lua` and for a
-- user config that may not exist; both are files here, just not on a tree.
--
function dofile(path)
  local src = fs.read(path) or fs.read(ROOT .. path:gsub("^.*/data/", ""))
  if not src then error("cannot open " .. tostring(path)) end
  return load(src, "@" .. path, "t", _ENV)()
end

--
-- `debug`, which this sandbox also does without.
--
-- Lite XL uses it for error reporting - a traceback when something throws,
-- and `getinfo` to name the file a plugin came from. Neither needs the real
-- library to be *useful*; both need it to exist.
--
debug = {
  traceback = function(msg) return tostring(msg or "") end,
  getinfo   = function() return { source = "@?", short_src = "?",
                                  currentline = 0, what = "Lua" } end,
}

ARGS, PLATFORM, ARCH = { "lite-xl" }, "Kosmos", "aarch64-kosmos"
EXEFILE, HOME, SCALE = "/lite-xl/lite-xl", "/home", 1

local src = fs.read(ROOT .. "core/start.lua")
local ok, err = pcall(load(src, "@core/start.lua", "t", _ENV))
print("start.lua: " .. tostring(ok) .. " " .. tostring(err))

local ok2, core = pcall(require, "core")
print("require core: " .. tostring(ok2) .. " " .. tostring(ok2 and "ok" or core))

if ok2 then
  local ok3, err3 = pcall(core.init)
  print("core.init(): " .. tostring(ok3) .. " " .. tostring(err3))
end
