-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_ShowImage
-- A picture, in a window.
--
--   wm photo                     the test pattern
--   wm photo:image00.png         something else in assets/images/
--   wm photo:/home/holiday.png   a file on the disk
--
-- The picture is named, not carried. This program never holds a pixel of
-- it: it tells the window manager which picture to draw and where, and the
-- window manager - which owns every pixel on the screen already - decodes
-- it once and blits it. That is the same arrangement that lets a hung
-- application keep a window.
--
-- **A name with a slash in it is a file; anything else is an asset.** This
-- program does not know the difference and does not need to - `ui.image`
-- passes the name through and the compositor decides where to look. Which
-- is why opening a photograph off the disk was a File menu here and one
-- branch there, rather than a picture travelling through messages.
--
-- Drag it, or use the arrows, if it is bigger than its frame. **The scaler
-- this comment waited for exists** - `s:stretch`, added on 15 September for
-- Music's covers - so fitting a photograph to its frame is now a change to
-- this program rather than something the system cannot do.

local ui    = use("/lib/ui.lua")
local panel = use("/lib/panel.lua")
local files = use("/lib/files.lua")

-- The *kit's* palette, not a copy of it.
--
-- `use` runs the chunk again and hands back a different table, and only the
-- one `ui.lua` holds is the one it mutates when the desktop changes theme.
-- An application that loaded its own kept the colours it started with while
-- every widget around it changed - which is exactly what Monitor, Processes,
-- Photo and the Terminal did.
local theme = ui.theme

local name = tostring(args or ""):match("^%s*(%S+)") or "test-pattern.png"

-- An asset is allowed to be named without its extension, because they are a
-- known short list. A path is not: guessing at one would turn a typo into a
-- different file.
--
-- `.png` is what a bare name gets, because that is what almost every asset
-- is. A JPEG asset has to be named in full, which is the right way round:
-- the guess should favour the common case and never silently find a
-- different file than the one that was asked for.
if not name:find("/") and not name:match("%.%a+$") then
  name = name .. ".png"
end

local W, H = 560, 420

-- The header band, the same numbers every other window in the new look
-- uses. `docs/desktop.html` calls this shape a *canvas*: the picture is the
-- window, and above it one row saying what you are looking at.
local TOOLBAR_Y = 7
local TOOLBAR_H = 26
local BAR_H     = TOOLBAR_Y + TOOLBAR_H

local win, err = ui.window{ title = "Photo", w = W, h = H, x = 110, y = 70,

  -- A picture dragged out of Tracker opens here. Which is the shortest
  -- description there is of what a drop is for.
  drops = true,
}

if not win then
  print("photo: " .. tostring(err))
  return
end

local where   = ui.label{ x = 12, y = TOOLBAR_Y + 5, w = W - 70, text = name,
                          color = "text_dim" }
local picture = ui.image{ x = 10, y = BAR_H + 8, w = W - 20,
                          h = H - BAR_H - 44, asset = name,
                          follow = { "left", "right", "top", "bottom" } }
local status  = ui.label{ x = 10, y = H - 26, w = W - 20, text = "",
                          follow = { "left", "bottom" } }

local function describe()
  if picture.image_w > 0 then
    return ("%d x %d, drag to pan"):format(picture.image_w, picture.image_h)
  end

  if name:find("/") then
    return "that file is not a picture this can decode"
  end

  return "not carried in the image"
end

--
-- Showing a different one, which is the whole of Open.
--
-- The window is retitled as well as the label changed, because the Deskbar
-- lists windows by title and a row of four saying "Photo" is a row of four
-- that tells you nothing.
--
local list_assets            -- defined with the label it fills

local function show(path)
  name = path
  where.text = path

  picture:set(path)
  status.text = describe()

  if list_assets then list_assets() end

  -- The Deskbar lists windows by title, and four of them saying "Photo" is
  -- a list that tells you nothing.
  win:retitle("Photo - " .. (path:match("([^/]+)$") or path))
end

local function open_one()
  local chooser = panel.open{
    start = name:find("/") and (name:match("^(.*)/") or "/home") or "/home",
    title = "Open a picture",
    on_choose = function(chosen) show(chosen) end,
  }

  if chooser then chooser:run() end
end

--
-- Dropped from Tracker, which sends one path per line. Only the first is
-- opened: this window shows one picture, and opening four of them into it
-- would show the last and silently discard three.
--
function win:on_drop(kind, payload)
  if kind ~= "files" then return false end

  local first = payload:match("^[^\n]+")

  if not first then return false end

  show(first)

  local rest = select(2, payload:gsub("\n", "\n"))

  if rest > 0 then
    status.text = describe() .. ("  (%d more not opened)"):format(rest)
  end

  return true
end

--
-- **A `...` instead of a File menu**, which is the whole of what a window
-- this quiet needs: two items, neither of them something anybody does
-- twice in a row (`docs/desktop.html`).
--
win:add(ui.button{
  x = W - 46, y = TOOLBAR_Y, w = 34, h = TOOLBAR_H, text = "...",
  follow = { "right", "top" },
  on_click = function()
    if not win.open_menu then return end

    win:open_menu(win.origin_x + W - 46,
                  win.origin_y + TOOLBAR_Y + TOOLBAR_H, {
      { text = "Open...", on_choose = open_one },
      { separator = true },
      { text = "Set as wallpaper", on_choose = function()
          if not name:find("/") then
            status.text = "only a file can be the wallpaper"
            return
          end

          local ok, why = fs.send("/app/wm", { type = "wallpaper",
                                              path = name })

          status.text = ok and "that is the desktop now"
                        or ("wallpaper: " .. tostring(why))
        end },
    })
  end,
})

win:add(where)
win:add(picture)
win:add(status)

--
-- What else there is, so the list is discoverable from inside the system
-- rather than only from the source tree.
--
-- **Only while an asset is being shown.** Once a real file is open the list
-- is answering a question nobody asked, and it was answering it in text
-- that ran off the right-hand edge of the window - which is what a label
-- with more in it than room does.
--
local also = ui.label{ x = 240, y = H - 26, w = W - 250, text = "",
                       color = "line", follow = { "left", "right", "bottom" } }

function list_assets()
  if name:find("/") then also.text = "" return end

  local others = {}

  for _, a in ipairs(sys.asset()) do
    if a ~= name then others[#others + 1] = a end
  end

  also.text = (#others > 0) and ("also: " .. table.concat(others, "  ")) or ""
end

win:add(also)

-- The state it opens in, whatever was named on the command line. `show`
-- rather than four assignments, so opening from the command line and
-- opening from the File menu leave the window in exactly the same state -
-- which is how the title came to be right in one case and stale in the
-- other.
if name:find("/") then show(name) else status.text = describe() end

list_assets()
win:run()
