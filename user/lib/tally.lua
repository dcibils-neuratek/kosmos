-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- How much is in a folder, counted a little at a time (`roadmap.md` 6za).
--
-- Info shows a folder's size, and a folder's size is everything inside it:
-- there is no number to ask for, only a walk. A walk of a drive can be
-- thousands of files, and a window that walked before it drew would be a
-- window that did not open. So this is a walk that stops: `step(n)` looks
-- at `n` more things and returns, the window draws what it has so far, and
-- the numbers climb while it is open - the "Instant feedback" rule: paint
-- from what is known at once, and fill in.
--
-- `store` is anything answering `list(path)` and `getattr(path)`: `fs` in
-- the Info window, a table in `tools/test_tally.lua`.
--
--   local t = tally.new(fs, { "/Home/roms" })
--   while not t:step(64) do ... draw t.files, t.folders, t.bytes ... end
--
-- **What is counted.** Everything *below* each path given; a path given
-- that is a file counts as a file. When several are given, each folder
-- among them counts as a folder too - "3 items" is three things, and two
-- of them being files and one a folder is what "2 files in 1 folder" says.
-- One folder given is the thing being described, and "1,204 files in 37
-- folders" is what is inside it.

local tally = {}

local methods = {}
methods.__index = methods

local function join(dir, name)
  return (dir == "/") and ("/" .. name) or (dir .. "/" .. name)
end

function tally.new(store, paths)
  local self = setmetatable({
    store = store,
    files = 0, folders = 0, bytes = 0,
    unreadable = 0,          -- folders that would not list
    pending = {},            -- paths still to look at
    done = false,
  }, methods)

  local several = #paths > 1

  for _, path in ipairs(paths) do
    local attrs = store.getattr(path) or {}

    if attrs.kind == "directory" then
      if several then self.folders = self.folders + 1 end
      self.pending[#self.pending + 1] = path
    else
      self.files = self.files + 1
      self.bytes = self.bytes + (tonumber(attrs.size) or 0)
    end
  end

  self.done = (#self.pending == 0)

  return self
end

--
-- Look at up to `budget` more things - a listing or an entry each - and say
-- whether the walk is finished. Deepest first, from a stack, so a folder's
-- inside is finished before its neighbour is started and the numbers settle
-- a folder at a time.
--
function methods:step(budget)
  local left = budget or 64

  while left > 0 and not self.done do
    local job = self.pending[#self.pending]

    if type(job) == "string" then
      -- A folder not listed yet: its names become the jobs.
      self.pending[#self.pending] = nil

      local names = self.store.list(job)

      if not names then
        self.unreadable = self.unreadable + 1
      else
        for i = #names, 1, -1 do
          self.pending[#self.pending + 1] = { join(job, names[i]) }
        end
      end
    else
      -- One name: a folder to list later, or a file to add in.
      self.pending[#self.pending] = nil

      local path = job[1]
      local attrs = self.store.getattr(path) or {}

      if attrs.kind == "directory" then
        self.folders = self.folders + 1
        self.pending[#self.pending + 1] = path
      else
        self.files = self.files + 1
        self.bytes = self.bytes + (tonumber(attrs.size) or 0)
      end
    end

    left = left - 1
    self.done = (#self.pending == 0)
  end

  return self.done
end

return tally
