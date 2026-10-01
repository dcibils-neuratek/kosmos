-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- What opens what.
--
-- One table, in one place, so that Tracker, the file panel and anything
-- else that opens a file agree - the alternative is each of them holding an
-- opinion and a `.md` opening in the editor from one and the reader from
-- another.
--
-- **By extension, and that is the temporary half.** BeOS did this properly:
-- a file's *type* was an attribute of the file, set when it was written and
-- travelling with it, and a separate table mapped a type to its preferred
-- application. A name is a much weaker thing to ask - renaming a file
-- changes what opens it, and a file with no extension has no type at all.
--
-- The machinery for the real version half exists. `kfs`'s inode carries an
-- `attrs` block that nothing writes yet, and the ramfs has typed attributes
-- already; when the disk grows attributes, `kind_of` reads the type
-- attribute first and falls back to the extension for files that were
-- written before anything set one. That fallback is why this is worth
-- building now rather than waiting.

local filetypes = {}

--
-- **What opens what, from the applications themselves** (`roadmap.md` 6z,
-- `docs/rightclick.html`). Each says it in its header - `-- kosmos: opens
-- png jpg jpeg` - as it says its icon; the store serving it reports the
-- words (`opens`), and this gathers them from where applications live. The
-- table this used to be, written by hand, is gone: it said `.wav` opened in
-- Play, which plays films, and nothing at all for a Doom level or a
-- cartridge that Doom and the Super Nintendo open.
--
-- `/Kosmos/Programs` as well as `/Kosmos/Apps`: Play is a program and opens
-- a film in a window of its own. And `/Home/Apps`, where an application is
-- installed - Doom, which opens a WAD (`docs/elf.md` step 5).
--
-- **A launcher** is a type too - `kind_of` says so from its attributes - and
-- the launcher editor opens it for editing; *starting* one is what Tracker
-- and the Deskbar do before they ever ask here.
--
filetypes.STORES = { "/Kosmos/Apps", "/Kosmos/Programs" }   -- then /Home/Apps

--
-- **A person's choice for a type**, where more than one application opens it
-- - written only when it differs from the default, so a home carried to
-- another machine takes its choices and nothing else. One setting, shown in
-- Preferences' File types and in Info; Open with in a right click is for
-- that once and changes nothing.
--
filetypes.CHOICES = "/Home/Preferences/filetypes"

--
-- **Which comes first, where two of the applications Kosmos ships open one
-- type** - said here, once, rather than left to the order of their names,
-- which put Reader before Text Editor for `.md`. A Markdown document opens
-- where it is written, and Reader, which shows the guides, is in Open with
-- (`roadmap.md` 6zs, agreed on 28 September). A person's own choice still
-- comes before this.
--
filetypes.PREFERRED = { md = "texteditor" }

--
-- Type -> the programs that open it, the default first: the ones in
-- `/Kosmos/Apps` before the ones in `/Kosmos/Programs`, and in each by name,
-- so the answer never depends on the order anything was found in.
--
-- `store` answers `list` and `getattr`: `fs`, or a table in a test. Made once
-- a process for `fs` - the applications in the image do not change while it
-- runs - and each time for anything else.
--
local gathered = nil

function filetypes.table(store)
  if not store and gathered then return gathered end

  local from = store or fs
  local out = {}

  for _, dir in ipairs(filetypes.STORES) do
    local names = from.list(dir) or {}

    table.sort(names)

    for _, file in ipairs(names) do
      local attrs = from.getattr(dir .. "/" .. file) or {}
      local short = tostring(file):gsub("%.lua$", "")

      for _, ext in ipairs(attrs.opens or {}) do
        ext = tostring(ext):lower()
        out[ext] = out[ext] or {}
        out[ext][#out[ext] + 1] = short
      end
    end
  end

  -- And what the installed applications open, after the shipped ones:
  -- read from each one's header, since a disk keeps no `opens` of its own.
  for _, app in ipairs(filetypes.installed(from)) do
    for ext in tostring(filetypes.declared(app.source, "opens") or "")
                 :lower():gmatch("[%w_]+") do
      out[ext] = out[ext] or {}
      out[ext][#out[ext] + 1] = app.name
    end
  end

  for ext, program in pairs(filetypes.PREFERRED) do
    for i, one in ipairs(out[ext] or {}) do
      if one == program and i > 1 then
        table.remove(out[ext], i)
        table.insert(out[ext], 1, program)
        break
      end
    end
  end

  if not store then gathered = out end

  return out
end

-- The programs that open a type, the default first; empty when none does.
function filetypes.openers(ext, store)
  return filetypes.table(store)[tostring(ext or ""):lower()] or {}
end

-- The choices a person has made, `{ mp4 = "play" }`; `read` is `fs.read`.
function filetypes.choices(read)
  local saved = (read or fs.read)(filetypes.CHOICES)

  return type(saved) == "table" and saved or {}
end

--
-- **Choose what opens a type**: written when it is not the default and
-- taken out when it is, as every setting here is kept. False and why when
-- the program does not open that type.
--
function filetypes.choose(ext, program, store, read, write)
  read, write = read or fs.read, write or fs.write
  ext = tostring(ext or ""):lower()

  local list = filetypes.openers(ext, store)
  local known = false

  for _, one in ipairs(list) do
    if one == program then known = true end
  end

  if not known then
    return nil, ("%s does not open .%s"):format(tostring(program), ext)
  end

  local saved = filetypes.choices(read)

  saved[ext] = (program ~= list[1]) and program or nil

  if not store and not fs.getattr("/Home/Preferences") then
    fs.send("/Home/Preferences", { type = "mkdir" })
  end

  return write(filetypes.CHOICES, saved)
end

-- What a path is, as a type name rather than a program.
function filetypes.kind_of(path, attrs)
  -- The attribute wins when there is one, because it was set by whoever
  -- wrote the file and a name is only a guess about it. Nothing writes one
  -- yet; this is the branch that will matter and it is here so that adding
  -- attributes to the disk does not mean revisiting every caller.
  if attrs and attrs.type then return attrs.type end

  --
  -- And a launcher is one, whoever wrote it.
  --
  -- `kind` is what the node *is* - file, directory, launcher - and for a
  -- launcher that is also the whole of what it is worth saying. Reading it
  -- here as well as `type` means every launcher already on a disk is a
  -- launcher to this function without anybody rewriting its attributes:
  -- there were thirty-eight of them on the first machine this ran on, and a
  -- migration to teach them a word they already knew would have been work
  -- for nothing.
  --
  -- New ones set `type` too, so the general branch above stays the one that
  -- matters and this is only the floor under it.
  --
  if attrs and attrs.kind == "launcher" then return "launcher" end

  local name = tostring(path):match("([^/]+)$") or ""

  -- A leading dot is not an extension. `.appearance` is a settings file
  -- whose whole name happens to start with one, and reading it as a file
  -- of type "appearance" put a type in Tracker's Kind column that nothing
  -- in the system has ever heard of.
  local ext = name:sub(2):match("%.([%w]+)$")

  return ext and ext:lower() or nil
end

--
-- Which program opens a path, or nil if nothing claims it.
--
-- Tracker has always called this and this file has never had it, so opening
-- a file from the file manager failed on the call rather than on the answer:
-- `types.opener(full)` on a nil field, every time, for every file. Nothing
-- caught it because nothing tests opening a file from Tracker - the display
-- harness starts applications from the Deskbar, which goes a different way.
--
-- Thin on purpose. `kind_of` already decides what a file *is*, attribute
-- first and extension second, and this is only the lookup from that to a
-- program name. Keeping them apart is what lets the type come from an
-- attribute later without this function changing at all.
--
--
-- **What a kind of file is called**, for Info's line under the name and the
-- File types page that 6z draws: the words a person would use, not the
-- extension read aloud. An extension with no words here is named by itself
-- - "SFC file" - which is honest and never wrong.
--
filetypes.names = {
  lua = "Lua source", txt = "Text", md = "Note", conf = "Settings", log = "Log",
  pdf = "PDF document", html = "Web page",
  png = "Picture", jpg = "Photograph", jpeg = "Photograph",
  mp3 = "Song", wav = "Sound", mp4 = "Film",
  wad = "Doom level", sfc = "Super Nintendo cartridge",
  smc = "Super Nintendo cartridge", zip = "Archive", theme = "Look",
  scene = "Cafesa3D scene", gltf = "3D scene", glb = "3D scene",
  favorite = "Favorite",
}

function filetypes.describe(path, attrs)
  attrs = attrs or {}

  if attrs.kind == "directory" then return "Folder" end
  if attrs.kind == "launcher" then return "Launcher" end

  -- A person's note in `/Home/Deskbar` that takes a shipped item out of the
  -- menu (`roadmap.md` 6zd), said as what it is: deleting it brings the
  -- item back, and Tracker is where somebody would look for why it went.
  if attrs.kind == "hidden" then return "Hidden from the Deskbar's menu" end
  if attrs.kind == "kit" then return "Kit, part of the system" end
  if attrs.kind == "application" then return "Application" end
  if attrs.kind == "program" then return "Program" end

  local ext = filetypes.kind_of(path, attrs)

  if not ext then return "File" end

  return filetypes.names[ext] or (ext:upper() .. " file")
end

--
-- **The File types page** (`roadmap.md` 6z, `docs/rightclick.html`): every
-- type something opens, as Preferences' rows - in the drawing's groups,
-- each type's name with what else opens it under it, and a choice at the
-- right where there is one, or the one application's name where there is
-- not. Two types that are one thing - `.jpg` and `.jpeg`, a cartridge's
-- two spellings - share a row, and a choice in it is made for both.
--
-- `filter` keeps the rows whose type, name or applications have it in them,
-- for the page's Find field. The rows are Preferences' own shape, with a
-- `tag` - the type, in its own column - and a `set` that makes the choice
-- through `choose` rather than a key written by hand.
--
filetypes.GROUPS = {
  { "Documents", { "txt", "md", "pdf", "html", "lua", "conf", "log" } },
  { "Pictures", { "png", "jpg", "jpeg" } },
  { "Sound and film", { "mp3", "wav", "mp4" } },
  { "Games", { "wad", "sfc", "smc" } },
  { "Kosmos", { "zip", "launcher", "favorite" } },
}

-- What only a sentence says about a type: opening a Lua file runs it.
local TOLD = {
  lua = "Opening runs it; this is what Edit uses",
  launcher = "Opening starts it; Edit uses this",
  favorite = "A page kept in /Home/Favorites; opening shows it",
  zip = "Opening extracts it",
}

function filetypes.page(store, read, filter)
  local all = filetypes.table(store)
  local placed, out = {}, {}
  local want = filter and tostring(filter):lower():gsub("^%s*%.?", "")
                                            :gsub("%s+$", "") or ""

  local function row(ext)
    local list = all[ext]
    local names = {}

    for i, program in ipairs(list) do names[i] = filetypes.app_name(program) end

    local others = {}

    for i = 2, #names do others[#others + 1] = names[i] end

    return {
      category = "filetypes", tag = "." .. ext, keys = { ext },
      label = filetypes.names[ext] or (ext:upper() .. " file"),
      notes = { TOLD[ext], (#others > 0)
                and (table.concat(others, " and ") .. " can open it too")
                or nil },
      kind = (#list > 1) and "choice" or "value",
      file = filetypes.CHOICES, key = ext, default = list[1],
      value = names[1], openers = list, names = names,
    }
  end

  local function same(a, b)
    return a.label == b.label and table.concat(a.openers, ",")
           == table.concat(b.openers, ",")
  end

  local function wanted(r)
    if want == "" then return true end

    local words = { r.label:lower() }

    for _, k in ipairs(r.keys) do words[#words + 1] = k end
    for _, n in ipairs(r.names) do words[#words + 1] = n:lower() end

    for _, w in ipairs(words) do
      if w:find(want, 1, true) then return true end
    end

    return false
  end

  local function finish(r)
    -- The second spelling of a shared row, said first, as the drawing does.
    local lines = {}

    if #r.keys > 1 then
      local also = {}

      for i = 2, #r.keys do also[#also + 1] = "." .. r.keys[i] end

      lines[#lines + 1] = table.concat(also, ", ") .. " the same"
    end

    for i = 1, 2 do
      if r.notes[i] then lines[#lines + 1] = r.notes[i] end
    end

    r.note = (#lines > 0) and table.concat(lines, " · ") or nil
    r.notes = nil

    if r.kind == "choice" then
      r.choices = {}

      for i, program in ipairs(r.openers) do
        r.choices[i] = { program, r.names[i] }
      end
    end

    local keys = r.keys

    r.set = function(program)
      for _, k in ipairs(keys) do
        local ok, why = filetypes.choose(k, program, store, read)

        if not ok then return nil, why end
      end

      return true
    end

    return r
  end

  local function group(name, exts)
    local rows = {}

    for _, ext in ipairs(exts) do
      if all[ext] and not placed[ext] then
        placed[ext] = true

        local r = row(ext)
        local last = rows[#rows]

        if last and same(last, r) then
          last.keys[#last.keys + 1] = ext
        else
          rows[#rows + 1] = r
        end
      end
    end

    local kept = {}

    for _, r in ipairs(rows) do
      if wanted(r) then kept[#kept + 1] = finish(r) end
    end

    for _, r in ipairs(kept) do r.group = name end

    if #kept > 0 then out[#out + 1] = { name = name, items = kept } end
  end

  for _, g in ipairs(filetypes.GROUPS) do group(g[1], g[2]) end

  -- Whatever an application opens that no group names, last.
  local rest = {}

  for ext in pairs(all) do
    if not placed[ext] then rest[#rest + 1] = ext end
  end

  table.sort(rest)
  group("Other", rest)

  return out
end

--
-- **An opener by the name its window has**, for the right click's "Open -
-- Video" and Info's "Opens with". A program's name is a file's, and `pdfview`
-- is not what anybody calls it. 6z replaces this with what each
-- application says of itself in its header.
--
local APP_NAMES = {
  texteditor = "Text Editor", ide = "Kosmos IDE",
  reader = "Reader", photo = "Photo", pdfview = "PDF",
  video = "Video", browser = "Browser", music = "Music", play = "Play",
  launcheredit = "Launcher editor", terminal = "Terminal",
  snes = "Super Nintendo", doom = "Doom",
}

function filetypes.app_name(program)
  program = tostring(program or "")

  return APP_NAMES[program]
         or (program:sub(1, 1):upper() .. program:sub(2))
end

function filetypes.opener(path, attrs, store, read)
  local kind = filetypes.kind_of(path, attrs)
  local list = kind and filetypes.openers(kind, store) or {}

  if #list == 0 then return nil end

  -- The person's choice, while it still opens the type.
  local chosen = filetypes.choices(read)[kind]

  for _, one in ipairs(list) do
    if one == chosen then return chosen end
  end

  return list[1]
end

--
-- **What a program declares about itself**, in its opening comment block:
-- `kosmos: application`, for one, means it draws a window.
--
-- The rule `/bin`'s server reads, and it has to be the same rule or a program
-- would be an application in the Deskbar and a console program in Tracker
-- (`user/servers/binfs.c`): the block is every line from the top that is
-- empty or begins with `--`, and it ends at the first that is neither - so
-- the same words in a string further down declare nothing.
--
function filetypes.declares(source, word)
  for line in (tostring(source or "") .. "\n"):gmatch("(.-)\n") do
    if line ~= "" and line:sub(1, 2) ~= "--" then
      return false
    end

    if line:match("kosmos:%s*(%a+)") == word then
      return true
    end
  end

  return false
end

--
-- **What a program's header says after a word**: `declared(source,
-- "section")` is `demos` for Doom, or nil - the rest of that `kosmos:` line,
-- by the rule above, for a program no store has read for us: one installed
-- in `/Home/Apps`, which a disk holds as a plain file.
--
function filetypes.declared(source, word)
  for line in (tostring(source or "") .. "\n"):gmatch("(.-)\n") do
    if line ~= "" and line:sub(1, 2) ~= "--" then return nil end

    local said, rest = line:match("kosmos:%s*(%a+)%s*(.-)%s*$")

    if said == word then return rest end
  end

  return nil
end

--
-- **The applications installed in `/Home/Apps`** (`docs/elf.md` step 5):
-- each folder whose program - the Lua file named after it - says it is an
-- application, as `{ name, folder, program, header }`. What the Deskbar lists
-- and what opens what gathers, beside the ones Kosmos ships.
--
filetypes.INSTALLED = "/Home/Apps"

function filetypes.installed(store)
  local from = store or fs
  local out = {}
  local names = from.list(filetypes.INSTALLED) or {}

  table.sort(names)

  for _, folder in ipairs(names) do
    local name = tostring(folder):lower()
    local program = filetypes.INSTALLED .. "/" .. folder .. "/" .. name .. ".lua"
    local source = from.read(program)

    if type(source) == "string" and filetypes.declares(source, "application") then
      out[#out + 1] = { name = name, folder = folder, program = program,
                        source = source }
    end
  end

  return out
end

--
-- **How to open a path**: the program to start, and what to hand it - or nil
-- when nothing claims it.
--
-- A Lua file is a program, so opening one runs it. An application - its
-- opening comment says `kosmos: application` - starts as itself and opens its
-- own window. Anything else is a console program, and runs in a Terminal of
-- its own, which is where its output has somewhere to go. `source` is the
-- file's beginning, and only a Lua file needs it.
--
-- Editing one is still `opener`'s answer - the editor - which is what
-- Tracker's File menu asks when you choose Edit.
--
function filetypes.how_to_open(path, attrs, source)
  local kind = filetypes.kind_of(path, attrs)

  if kind == "lua" then
    if filetypes.declares(source, "application") then
      return { program = path, args = "" }
    end

    return { program = "terminal", args = path }
  end

  local program = kind and filetypes.opener(path, attrs)

  return program and { program = program, args = path } or nil
end

return filetypes
