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
-- **A picture bigger than its frame is fitted to it**, keeping its shape,
-- through the compositor's scaler (`s:stretch`, added on 15 September for
-- Music's covers); a smaller one sits in the middle at its own size. This
-- said to drag a big one around, and waited for the scaler to do better -
-- it did, in 0.10.149 (`ui.image`'s `contain`).

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

-- **A canvas** (`docs/apps.html`): the kit's header with the picture's name
-- and size, and under it the picture in the middle of a dark ground that
-- runs to the window's edges. It was a row 33 tall, a sunken well 10 in
-- with the picture in its top-left corner, and two lines of status under
-- it; what they said is said beside the name, or behind the dots.
local L = ui.layout

local win, err = ui.window{ title = "Photo", w = W, h = H, x = 110, y = 70,

  -- A picture dragged out of Tracker opens here. Which is the shortest
  -- description there is of what a drop is for.
  drops = true,
}

if not win then
  print("photo: " .. tostring(err))
  return
end

local function base(p) return p:match("([^/]+)$") or p end

local more = ui.iconbutton{ icon = "more" }
local header = ui.header{ x = 0, y = 0, w = W, title = base(name), sub = "",
                          right = { more } }
local picture = ui.image{ x = 0, y = L.head, w = W, h = H - L.head,
                          asset = name, ground = theme.console,
                          contain = true, centre = true,
                          follow = { "left", "right", "top", "bottom" } }

-- What was said last goes beside the name: the header is the status line.
local function say(text) header.sub = text end

local function describe()
  if picture.image_w > 0 then
    return ("%d × %d"):format(picture.image_w, picture.image_h)
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
local function show(path)
  name = path
  header.title = base(path)

  picture:set(path)
  say(describe())

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
    say(describe() .. (" · %d more not opened"):format(rest))
  end

  return true
end

--
-- **The dots instead of a File menu**, which is the whole of what a window
-- this quiet needs: two items, neither of them something anybody does
-- twice in a row (`docs/desktop.html`).
--
-- **And the pictures the system carries, under them, while one of those is
-- what is shown** - so the list is discoverable from inside the system
-- rather than only from the source tree. It was a line of names along the
-- window's bottom that ran off its right-hand edge; a menu has room for
-- them, and choosing one is showing it.
--
more.on_click = function()
  local items = {
    { text = "Open...", on_choose = open_one },
    { separator = true },
    { text = "Set as wallpaper", on_choose = function()
        if not name:find("/") then
          say("only a file can be the wallpaper")
          return
        end

        local ok, why = fs.send("/app/wm", { type = "wallpaper",
                                            path = name })

        say(ok and "that is the desktop now"
            or ("wallpaper: " .. tostring(why)))
      end },
  }

  if not name:find("/") then
    local first = true

    for _, a in ipairs(sys.asset()) do
      if a ~= name and not a:find("/") and a:match("%.%a+$")
         and #items < 16 then
        if first then items[#items + 1] = { separator = true } end
        first = false
        items[#items + 1] = { text = a, on_choose = function() show(a) end }
      end
    end
  end

  win:open_menu(win.origin_x + more.x, win.origin_y + L.head, items)
end

win:add(picture)
win:add(header)

-- The state it opens in, whatever was named on the command line. `show`
-- rather than four assignments, so opening from the command line and
-- opening from the File menu leave the window in exactly the same state -
-- which is how the title came to be right in one case and stale in the
-- other.
if name:find("/") then show(name) else say(describe()) end

win:run()
