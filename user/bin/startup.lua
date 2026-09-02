-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- What opens when the desktop does.
-- kosmos: application
-- kosmos: section preferences
--
-- A list of everything that can be started, with a box beside each. Tick
-- one and it opens the next time the desktop comes up.
--
-- **The desktop's list, not the machine's**, and the difference is the whole
-- design. `init.lua` already has a way to run something at boot -
-- `-fw_cfg opt/kosmos/boot` - and says in as many words why it is a command
-- line option and not a file: *a machine that will not reach a prompt
-- because of something written in a file is a machine you cannot fix from
-- the prompt.* That argument is right and it applies here, so this does not
-- go anywhere near it.
--
-- What this writes is read by the Deskbar, after the desktop is up and
-- after there is a shell. Tick something that crashes on sight and you get
-- a desktop with one dead application on it, which you can then untick.
-- The machine boots either way, because nothing on the boot path ever
-- reads this file.
--
-- The Deskbar is the reader for the same reason it owns the menu: it is
-- what starts applications. The window manager knows how to composite and
-- should not learn a policy about which programs a person likes.

local ui = use("/lib/ui.lua")

local SETTINGS = "/home/.startup"

local W, H = 320, 340

local win, err = ui.window{ title = "Startup", w = W, h = H, x = 260, y = 120 }

if not win then
  print("startup: " .. tostring(err))
  return
end

--
-- Everything that can be started, which is exactly what the Deskbar lists.
--
-- Read from the program store rather than kept in a table here, so an
-- application dropped into `/bin` appears in both without either being
-- edited - and so this cannot offer to start something that is not there.
--
local names = {}

for _, file in ipairs(fs.list("/bin") or {}) do
  local attrs = fs.getattr("/bin/" .. file)

  if attrs and attrs.kind == "application" then
    local short = file:gsub("%.lua$", "")

    -- Not the Deskbar, which is not something you start, and not this
    -- window: a preferences panel that opened itself on every boot would be
    -- a mistake you could only undo by making it.
    if short ~= "deskbar" and short ~= "startup" then
      names[#names + 1] = short
    end
  end
end

table.sort(names)

--
-- What is ticked. A set keyed by name, which is what `ui.list` reads.
--
-- Stored as a *list* rather than as the set, because a set has no order and
-- the order is a real choice: things you start at login open in front of
-- each other, and the last one is the one you are looking at.
--
local ticked = {}
local saved = fs.read(SETTINGS)

if type(saved) == "table" and type(saved.items) == "table" then
  for _, name in ipairs(saved.items) do ticked[tostring(name)] = true end
end

local status = ui.label{ x = 12, y = H - 30, w = W - 24, text = "" }

--
-- Written on every tick rather than behind a Save button.
--
-- Same choice `appearance` makes, for the same reason: there is one setting
-- here and no way to be halfway through changing it, so a Save button would
-- only be a way to lose what you just did.
--
local function save()
  local items = {}

  -- In the order they are shown, so the file reads the way the window looks.
  for _, name in ipairs(names) do
    if ticked[name] then items[#items + 1] = name end
  end

  local ok, why = fs.write(SETTINGS, { items = items })

  if not ok then
    status.text = "not saved: " .. tostring(why)
  elseif #items == 0 then
    status.text = "nothing starts with the desktop"
  else
    status.text = ("%d at startup: %s"):format(#items,
                                               table.concat(items, ", "))
  end
end

win:add(ui.label{ x = 12, y = 10, w = W - 24,
                  text = "Open these when the desktop starts" })

win:add(ui.list{
  x = 12, y = 10 + gfx.font.h + 6, w = W - 24, h = H - 90 - gfx.font.h,
  items = names,
  checks = ticked,
  on_toggle = save,
})

win:add(status)

save()
win:run()
