-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- prefs: an application's settings, kept as text in /Home/Preferences.
--
--   local prefs = use("/Kosmos/Libraries/prefs.lua")
--   local music = prefs.open("music", { volume = 80, shuffle = false })
--
--   music.volume                -- 80 until it is changed
--   music:set("volume", 70)     -- written at once, every other key kept
--   music:set{ shuffle = true, volume = 60 }
--   music:reset("volume")       -- back to the application's default
--
-- Diego, 4 October 2026: "There should be a settings kit that allows an app
-- to store and read settings", "So apps use that kit instead of inventing
-- their own way". Every application had its own: a path written out, its
-- defaults merged by hand, the read-modify-write that keeps what another
-- program put in the same file, the folder made if it was missing. This is
-- all of it, once - and `make host-check` fails a build in which anything
-- in `user/bin`, `user/lib` or `user/installed` but this file names the
-- folder itself.
--
-- **A file is one application's**, named by one word - or a word and a word
-- under it, `browser/settings` - and never `..`, so nothing reaches past
-- `/Home/Preferences`. It is text (`tabletext`, `design.md` 8.3e): a person
-- can open it, read it and change it.
--
-- **A default is the application's, not the file's.** A setting nobody
-- changed is not written, so it follows the application when its default
-- does; a setting set back to its default leaves the file. What the file
-- holds is what a person chose.
--
-- **Read, change, write** on every `set`, never a whole file from what this
-- process happens to remember: Preferences and the application can keep one
-- file between them and neither loses the other's keys.
--
-- A field read is the setting, so a setting named like a method - `get`,
-- `set`, `reset`, `all`, `reload`, `path` - is read with `:get(name)`.

local prefs = {}

prefs.DIR = "/Home/Preferences"

local WORD = "[%w][%w_%.%-]*"

-- `name` as a settings file's name, or an error that says what one is.
local function checked(name, level)
  name = tostring(name or "")

  local ok = (name:match("^" .. WORD .. "$") or name:match("^" .. WORD .. "/" .. WORD .. "$"))
             and not name:find("..", 1, true)

  if not ok then
    error(("prefs: %q is not a settings name - a word, or a word/word"):format(name),
          (level or 1) + 1)
  end

  return name
end

-- Where `name` is kept. For what has to be handed a path - a watch, an
-- attribute - rather than for reading or writing it by hand.
function prefs.path(name)
  return prefs.DIR .. "/" .. checked(name, 2)
end

-- The folder made, and every one above it, by the file library's one way
-- of doing that (`files.make_folder`). It made `/Home/Preferences` and the
-- folder asked for, and not one between: `browser/history` on a disk with
-- no `browser` was a mkdir the filesystem refused. Reached when a file is
-- first written, so a program that only reads its settings does not load it.
local function ensure(dir)
  return use("/Kosmos/Libraries/files.lua").make_folder(dir) == true
end

-- A folder an application keeps files of its own in - the browser's
-- history, the certificates a person trusts - made if it is not there.
function prefs.folder(name)
  local path = prefs.DIR .. "/" .. checked(name, 2)

  ensure(path)
  return path
end

-- The table kept under `name`, as it is - no defaults - or `{}` when there
-- is none. A file broken by hand reads as `{}`, with why beside it.
function prefs.read(name)
  local got, why = fs.read(prefs.DIR .. "/" .. checked(name, 2))

  if type(got) == "table" then return got end

  return {}, (got ~= nil or why ~= nil) and tostring(why or "not a table") or nil
end

-- A whole table kept under `name`, replacing what was there: for a file that
-- is a list rather than settings - the tabs that were open, the choices of
-- what opens what. True, or nil and why.
function prefs.write(name, t)
  local path = prefs.DIR .. "/" .. checked(name, 2)

  ensure(path:match("^(.*)/[^/]+$"))

  local ok, why = fs.write(path, t)

  if not ok then return nil, tostring(why) end

  return true
end

--------------------------------------------------------------------------
-- An application's settings.
--------------------------------------------------------------------------

local methods = {}

local handle = {}

handle.__index = function(self, key)
  local m = methods[key]

  if m then return m end

  return methods.get(self, key)
end

function methods.get(self, key)
  local v = rawget(self, "_saved")[key]

  if v == nil then v = rawget(self, "_defaults")[key] end

  return v
end

-- Every setting, the defaults under what was chosen: a copy.
function methods.all(self)
  local out = {}

  for k, v in pairs(rawget(self, "_defaults")) do out[k] = v end
  for k, v in pairs(rawget(self, "_saved")) do out[k] = v end

  return out
end

function methods.reload(self)
  rawset(self, "_saved", (prefs.read(rawget(self, "_name"))))
  return self
end

function methods.path(self)
  return prefs.DIR .. "/" .. rawget(self, "_name")
end

-- `set(key, value)` or `set{ key = value, ... }`: read, changed, written.
-- A value equal to its default leaves the file, and so does `nil` - which a
-- table of changes cannot carry, so one key at a time is told apart from it.
-- True, or nil and why.
function methods.set(self, key, value)
  local name = rawget(self, "_name")
  local defaults = rawget(self, "_defaults")
  local saved = prefs.read(name)

  local function change(k, v)
    if v == nil or (type(v) ~= "table" and v == defaults[k]) then
      saved[k] = nil
    else
      saved[k] = v
    end
  end

  if type(key) == "table" then
    for k, v in pairs(key) do change(k, v) end
  else
    change(key, value)
  end

  local ok, why = prefs.write(name, saved)

  if ok then rawset(self, "_saved", saved) end

  return ok, why
end

-- A setting back to the application's default.
function methods.reset(self, key)
  local name = rawget(self, "_name")
  local saved = prefs.read(name)

  saved[key] = nil

  local ok, why = prefs.write(name, saved)

  if ok then rawset(self, "_saved", saved) end

  return ok, why
end

-- `name`'s settings over `defaults`: read now, and again by `reload`.
function prefs.open(name, defaults)
  name = checked(name, 2)

  local self = { _name = name, _defaults = defaults or {} }

  self._saved = prefs.read(name)

  return setmetatable(self, handle)
end

return prefs
