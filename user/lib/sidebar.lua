-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The sidebar Tracker and every Open and Save window share: Places, System, Drives.
--
-- `drives.html`: "An app's Open and Save windows have the same sidebar as
-- Tracker: Places and Drives. You go through whichever is quicker." The same
-- sidebar, so it is the same code - moved here out of `tracker.lua` on 18
-- September as it was, comments and all, when the Open window became its
-- second user (USB step 6d).
--
-- What it builds is the rows `ui.tree` draws. What a caller does with a row -
-- go there, open a file, make a place of a drop - stays the caller's, because
-- Tracker makes and removes places and an Open window only gets around.
--
--   local side = sidebar.new()
--   ui.tree{ ..., roots = side.roots() }
--   side.refresh()              -- after a place is made, and on Refresh
--   side.volumes(true)          -- what /drives answers now, not at last draw
--
-- One per window, because the volume cache and the groups a refresh clears
-- belong to the tree that drew them.

local files    = use("/lib/files.lua")
local placelib = use("/lib/places.lua")

local sidebar = {}

local function subdirs(node)
  local out = {}

  for _, e in ipairs(files.entries(node.path) or {}) do
    if e.kind == "directory" then
      local full = files.join(node.path, e.name)

      out[#out + 1] = { text = e.name, path = full, children = subdirs }
    end
  end

  return out
end

--
-- The roots are the mounts this process was handed, asked for rather than
-- listed here.
--
-- **Six strings used to be written out at this spot, and they fell
-- behind.** `/user`, `/app` and `/ramfs` are all mounted and none of them
-- was offered, so a file manager could not reach places its own process
-- could see - which reads as the system hiding things from you and is
-- really a hardcoded list going stale. The rename of `/data` left the
-- label saying `data` beside a path saying `/ramfs`, which is the same
-- rot one step further on.
--
-- Two filters, and both are about what a *browser* can use:
--
-- **A mount inside another one is not a root.** `/dev/console` and
-- `/dev/audio` are mounts and already appear inside `/dev`; offering them
-- again would be a pane that disagrees with the tree underneath it.
--
-- **A mount that cannot be listed is not offered.** `/net` is a protocol
-- rather than a tree and answers a listing with an error, and so does the
-- disk on a machine that has none. Asking is the only way to tell them
-- apart - nothing on a mount says "browsable" - and a root that did
-- nothing when clicked would be worse than one that is not there.
--
-- The order is the namespace's own, which is alphabetical. That is a
-- deliberate non-choice: any order picked here is one more thing to edit
-- the next time a mount appears.
--
local function mount_roots()
  local all = fs.mounts()
  local out = {}

  for _, prefix in ipairs(all) do
    local nested = false

    for _, other in ipairs(all) do
      if other ~= prefix and prefix:sub(1, #other + 1) == other .. "/" then
        nested = true
        break
      end
    end

    --
    -- **It used to ask each mount for a listing first, and that is what
    -- wedged the first real machine.**
    --
    -- The probe was one round trip per mount to decide whether the mount
    -- was worth showing - "the question here is only whether the mount
    -- answers a listing at all". Twelve mounts, twelve round trips, on
    -- every Tracker startup, and Tracker is the desktop: if any one of them
    -- does not answer, the desktop does not exist.
    --
    -- One did not. `/net` on a laptop with no network card took **18.4
    -- seconds** to come back, measured, while every other mount answered in
    -- four to twelve milliseconds. Both Tracker processes sat in this loop;
    -- the compositor cycled happily with nothing to draw on; four other
    -- applications painted and polled and waited for a desktop that was
    -- inside a filesystem call. From the outside it was indistinguishable
    -- from a hung window manager, and it was a sidebar asking a question it
    -- did not need the answer to.
    --
    -- So it does not ask. Every mount is shown, and one that cannot be
    -- listed shows empty when it is opened - which is the right place to
    -- find that out, because by then a person has asked for it and is
    -- waiting for one directory rather than for the machine to start.
    --
    -- The deeper fault is still open: a filesystem call that takes eighteen
    -- seconds to fail is a bug wherever it lives, and this only stops the
    -- desktop being the thing that pays for it.
    --
    if not nested then
      out[#out + 1] = { text = prefix:sub(2), path = prefix,
                        children = subdirs }
    end
  end

  return out
end

--
-- **Three groups, in the order `docs/drives.html` draws them**: Places for
-- where you work, System folded away, and Drives for what is plugged in.
--
-- The groups are `heading` nodes, which `ui.tree` draws dim and refuses to
-- select: a heading names a set of places and is not one itself, so clicking
-- it neither highlights nor navigates.
--
-- **The design's list, not a guess at one.** `drives.html` folds away
-- `bin, lib, app, dev, net, ramfs`, and that is what System holds. An
-- earlier version here was a set literal of my own invention: it swallowed
-- `/system` and `/user` as well, so Places was left holding `user` - which
-- the drawing never mentions - and no `Desktop`, which it does.
local SYSTEM_MOUNTS = {
  ["/bin"] = true, ["/lib"] = true, ["/app"] = true,
  ["/dev"] = true, ["/net"] = true, ["/ramfs"] = true,
}

--
-- **The drives, asked for only when the group is opened.**
--
-- `mount_roots` above records what probing mounts at startup once cost: 18.4
-- seconds on a laptop with no network card, with the whole desktop looking
-- hung, because Tracker *is* the desktop. Asking `/drives` what is plugged
-- in is the same shape of question one mount further along, so it is asked
-- here - inside `children`, which `ui.tree` calls when somebody opens the
-- group - and never on the way to a first frame.
--
-- `fs.volumes` rather than a listing, because a listing gives names and then
-- costs a `getattr` for each one; this returns the filesystem, the size and
-- how much is free in a single call, which is what the rows want.
--
sidebar.subdirs = subdirs

function sidebar.new()
  --
  -- **What `/drives` answered, once per refresh**, shared by the Drives group
  -- and by any place on a drive. Both are drawn in the same pass, and asking
  -- twice would be the same question twice on the way to a frame.
  --
  local volumes_seen = nil

  local function volumes_now()
    if volumes_seen == nil then
      volumes_seen = (fs.volumes and fs.volumes("/drives")) or {}
    end

    return volumes_seen
  end

  local function drive_rows()
    local out = {}
    local volumes = volumes_now()

    for _, v in ipairs(volumes or {}) do
      --
      -- `note` is the quiet half of the row: `KOSMOS HOME` and then `kfs`.
      -- A volume this system cannot open says so there rather than being
      -- hidden, because a drive with a partition missing looks broken.
      --
      local note = v.filesystem or "unknown"

      if not v.readable then note = note .. ", not opened" end

      out[#out + 1] = { text = v.name, path = files.join("/drives", v.name),
                        note = note, children = subdirs }
    end

    if #out == 0 then
      --
      -- A group with nothing under it reads as broken; this says which of the
      -- two it is.
      --
      -- **Short on purpose.** The pane is 150 pixels and `gc:text` clips by
      -- whole character cells, so "(nothing plugged in)" came out as
      -- "(nothing plugge" - the same clipping that cost three wrong guesses on
      -- Music's footer (`testing.md` 18.85).
      --
      out[1] = { text = "(no drives)", quiet = true }
    end

    return out
  end

  --
  -- **Places is where you work**, and `drives.html` names it: Home, Desktop,
  -- and shortcut places. It is not "every mount that is not a system one" -
  -- that was an earlier guess here, and it put `user` in the list and left
  -- `Desktop` out.
  --
  -- **And the shortcuts a person made**, the drawing's `MyPhotos on PHOTOS
  -- 2024`: files in `/home/Places`, each found again by what its volume *is*
  -- rather than by its name or its unit, both of which change on a replug
  -- (`/lib/places.lua` has the rule and `tools/test_places.lua` the proof).
  --
  -- A place whose drive is away stays in the list, dimmed and saying so -
  -- `drives.html`: "Unplug the drive and MyPhotos stays in Places, greyed
  -- out". `quiet` is the tree's word for a row with nowhere to go. Only a
  -- place on a drive asks `/drives` anything, so a sidebar without one costs
  -- the first frame nothing.
  --
  local function place_rows()
    local out = {
      { text = "Home", path = "/home", children = subdirs },
      { text = "Desktop", path = "/home/Desktop", children = subdirs },
    }

    for _, p in ipairs(placelib.read(fs)) do
      local volumes = p.attrs.volume and volumes_now() or {}
      local path, called = placelib.resolve(p.attrs, volumes)

      if path then
        out[#out + 1] = { text = p.name, path = path, place = p,
                          note = called and ("on " .. called) or nil,
                          children = subdirs }
      else
        out[#out + 1] = { text = p.name, place = p, quiet = true,
                          note = called or "unplugged" }
      end
    end

    return out
  end

  local groups = {}

  local function grouped_roots()
    local system, other = {}, {}

    for _, m in ipairs(mount_roots()) do
      if m.path == "/drives" then
        -- The Drives group answers for it, with what each volume is.
      elseif SYSTEM_MOUNTS[m.path] then
        system[#system + 1] = m
      elseif m.path ~= "/home" then
        -- Everything else the process can reach, under System as well: it is
        -- somewhere you *can* go rather than somewhere you work, and hiding a
        -- mount a program holds would be the sidebar disagreeing with the
        -- namespace.
        other[#other + 1] = m
      end
    end

    for _, m in ipairs(other) do system[#system + 1] = m end

    --
    -- Places and Drives are fetched when drawn rather than now, so neither is
    -- asked on the way to Tracker existing - and each is kept so a refresh can
    -- clear just those two, leaving whatever else was opened open.
    --
    groups.places = { text = "Places", heading = true, open = true,
                      children = place_rows }
    groups.drives = { text = "Drives", heading = true, open = true,
                      children = drive_rows }

    return {
      groups.places,
      { text = "System", heading = true, kids = system },
      groups.drives,
    }
  end


  local self = {}

  self.roots = grouped_roots

  --
  -- **Read again**: after a place is made or removed, and on Refresh. Only
  -- Places and Drives, so a folder somebody opened in System stays open.
  --
  function self.refresh()
    volumes_seen = nil

    if groups.places then groups.places.kids = nil end
    if groups.drives then groups.drives.kids = nil end
  end

  --
  -- What `/drives` answers - fresh when asked for, because a place has to
  -- key on what is plugged in *now*, not at the last draw.
  --
  function self.volumes(fresh)
    if fresh then volumes_seen = nil end

    return volumes_now()
  end

  return self
end

return sidebar
