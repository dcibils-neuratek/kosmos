-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- JetSki's menu and the doors it shares with the keys, one of its files
-- (`jetski.lua`, *JetSki's files*): a function of `J`, giving back
-- `MENU` - `open`, under the three dots; `save_page`, `open_file`,
-- `zoom_to` and `zoom_step`, which a key and a row both call; and
-- `save_frame`, every pass.

return function(J)
  local files, prefs = J.files, J.prefs
  local MENU = {}
  local saving                       -- a save asked of WebKit, not answered yet

  -- **Save page as** (`docs/webkit.html`): Control-S opens the system's Save
  -- window at `/Home/Downloads`, named after the page's title. As a web page,
  -- complete - one MHTML file with its styles and pictures, which JetSki
  -- opens again without the network - or the page's HTML only, as it came.
  -- WebKit writes the bytes (`v:save`); what came of it is said when it says
  -- (`st.saved`, `save_frame`).
  --
  local function save_page()
    if not J.page.address or J.page.address == "" or J.page.address:match("^about:") then
      J.say("there is no page here to save")
      return
    end

    local folder = "/Home/Downloads"

    if not files.make_folder(folder) then folder = "/Home" end

    -- The title as a name: no slashes, nothing unprintable, not too long.
    local stem = tostring(J.page.title or ""):gsub("[/%c]", " "):gsub("%s+", " ")
                                           :match("^%s*(.-)%s*$"):sub(1, 80)

    if stem == "" then stem = J.page.address:match("^%a+://([^/:]+)") or "page" end

    local chooser = use("/Kosmos/Libraries/panel.lua").save{
      start = folder, title = "Save page as",
      name = files.free_name(folder, stem .. ".mhtml"),
      as = { { "complete", "A web page, complete - one file, with its pictures", "mhtml" },
             { "html", "The page's HTML only", "html" } },
      on_choose = function(path, as)
        J.win:focus_on(J.view)
        J.web.focus(true)
        saving = { path = path, as = as }
        J.web.save(path, as == "complete")
        J.say(("saving %s as %s"):format(path, as == "complete" and "a web page, complete"
                                                         or "its HTML"))
      end,
    }

    if chooser then chooser:run() end
  end

  -- What a save came to, said once WebKit has written it.
  local function save_frame()
    if not (J.st.saved and saving) then return false end

    local s = saving
    local bytes, why = J.web.saved()

    saving = nil

    if bytes then
      J.say(("saved %s, %d bytes, as %s"):format(s.path, bytes, s.as))
      use("/Kosmos/Libraries/notify.lua").post{ title = "Saved", body = s.path:match("[^/]+$") or s.path, open = s.path }
    else
      J.say(("could not save %s: %s"):format(s.path, tostring(why)))
    end

    return true
  end

  -- Control-O's: an HTML file, chosen in the system's Open window.
  function MENU.open_file()
    local chooser = use("/Kosmos/Libraries/panel.lua").open{
      -- Where the page shown is, when it is a file; /Home otherwise.
      start = (J.page.address or ""):match("^file://(/.*)/[^/]*$") or "/Home",
      title = "Open a web page",
      filter = function(name)
        local n = name:lower()
        return n:match("%.html?$") ~= nil or n:match("%.mhtm?l?$") ~= nil
      end,
      on_choose = function(path) J.win:focus_on(J.view) J.web.focus(true) J.visit(path) end,
    }
    if chooser then chooser:run() end
  end

  -- **The zoom, a step at a time**: Settings' four (Diego, 1 October, "just
  -- put 4 options"), Control-plus up one, Control-minus down one and
  -- Control-nought back to the first - and kept, as choosing it in Settings
  -- keeps it. `to` is 1, -1 or 0; `MENU.zoom_to` sets one of the four.
  function MENU.zoom_to(percent)
    if percent ~= J.setting.zoom then
      J.keep("zoom", percent)
      J.webkit.zoom(percent)
    end

    J.say(("zoom %d%%"):format(percent))
  end

  function MENU.zoom_step(to)
    local list, at = prefs.CHOICES.zoom, 1

    for i, z in ipairs(list) do
      if z[1] == J.setting.zoom then at = i end
    end

    MENU.zoom_to(((to == 0) and list[1] or list[math.max(1, math.min(#list, at + to))])[1])
  end

  -- **The menu** (`docs/webkit.html`, *The menu*): everything JetSki does,
  -- under the drawing's headings, each with its key - one door to each, so
  -- the row and the key do the same thing. A row whose feature is not built
  -- yet is drawn greyed, which says it is coming where a missing row says
  -- nothing; *Bookmark this page* is *Add to favorites*, since on Kosmos
  -- they are (`webkit.md`, *Doubts*).
  function MENU.items()
    local function heading(text) return { text = text, heading = true } end
    local function row(text, hint, fn) return { text = text, hint = hint, on_choose = fn } end
    local function later(text, hint) return { text = text, hint = hint, disabled = true } end
    local sep = { separator = true }

    local zooms = {}
    for _, z in ipairs(prefs.CHOICES.zoom) do
      zooms[#zooms + 1] = { text = z[2], mark = (z[1] == J.setting.zoom),
                            on_choose = function() MENU.zoom_to(z[1]) end }
    end

    local items = {
      heading("File"),
      row("New tab", "Ctrl T", J.new_tab),
      row("Open a file...", "Ctrl O", MENU.open_file),
      row("Save page as...", "Ctrl S", save_page),
      later("Print...", "Ctrl P"),
      sep, heading("Edit"),
      row("Find on the page...", "Ctrl F", function() J.F.open() end),
      sep, heading("View"),
      { text = ("Zoom: %d%%"):format(J.setting.zoom), hint = "Ctrl + - 0", submenu = zooms },
      later("Page source", "Ctrl U"),
      row("Downloads", "Ctrl Shift J", function() J.downloads.toggle() end),
      sep, heading("History and favorites"),
      later("History", "Ctrl Y"),
      row(J.FAV.star.lit and "Remove from favorites" or "Add to favorites", "Ctrl D", J.FAV.toggle),
      sep,
      row("Settings", "Ctrl ,", J.open_settings),
    }

    if J.DEVELOPER then
      table.insert(items, #items, row("Web Inspector", "Ctrl Alt I", J.toggle_inspector))
    end

    return items
  end

  -- Opened under the three dots, and said - the rows by where they are, for
  -- a suite to press one as a hand does.
  function MENU.open()
    local x, y = J.more:in_window()
    local items = MENU.items()
    local m = J.win:open_menu((J.win.origin_x or 0) + x, (J.win.origin_y or 0) + y + J.more.h, items)

    if m and m.row then
      local at, names = 2, {}
      for _, it in ipairs(items) do
        local h = it.separator and m.row // 3 or m.row
        if it.text and not it.heading then
          names[#names + 1] = ("%s%s at %d"):format(it.text, it.disabled and " (later)" or "", at + h // 2)
        end
        at = at + h
      end
      print(("jetski: the menu at %d,%d %dx%d: %s"):format(m.x or -1, m.y or -1, m.w or 0, m.h or 0,
                                                         table.concat(names, "; ")))
    end
  end

  MENU.save_page, MENU.save_frame = save_page, save_frame
  return MENU
end
