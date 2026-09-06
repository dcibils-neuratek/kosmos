-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: needs network
-- Fetches a page, parses it, and shows what came out.
--
--   wm browser
--
-- **This is not a rendered page and the window says so.** There is no
-- layout engine yet: what it shows is the *text* of the parsed document,
-- wrapped to the window, with the counts underneath. Calling that browsing
-- would be a lie, and the status line is written to make the difference
-- visible rather than to paper over it.
--
-- What it does prove, end to end and on the machine: a connection opens, an
-- HTTP request goes out, bytes come back, hubbub parses them into a DOM,
-- and libdom answers questions about the tree. Every piece of that was built
-- separately and this is the first thing that runs them together.
--
-- The chrome is the shape NetSurf's own is: back, reload, an address, and a
-- status line that says what the last load cost. It is here early *because*
-- the engine is unfinished - a window with somewhere for each piece to
-- appear is what makes the next piece visible when it lands.
--
-- **No DNS**, so an address is four numbers. `ping` and `fetch` say the same
-- thing for the same reason: nothing on this machine resolves a name yet.

local ui = use("/lib/ui.lua")
local theme = ui.theme

--
-- **No `gfx.use_font` here, and that was tried.**
--
-- It sets the face for the calling *process*, and this process does not
-- draw: `gc:text` puts an op in a list and the window manager rasterises
-- it, with the four faces `theme.lua` names. So an application asking for
-- a font asks the wrong process and nothing happens - which is exactly
-- what a screenshot showed, twice.
--
-- The face a page is drawn in is `theme.fonts.text`, which every role
-- defaults to `spleen`: an 8x16 bitmap with ASCII in it and nothing else.
-- That is why a page with accented Latin in it shows boxes here, and it is
-- an appearance setting rather than a browser bug.

local W, H = 900, 640
local BAR_H = gfx.font.h + 8

--
-- The kit is only in an image built with `WEB=1`, so its absence is an
-- ordinary state rather than a failure: the window opens and says which
-- build it is running on. An application that raised here would be a broken
-- entry in the Deskbar of every ordinary image.
--
local have, web = pcall(use, "/kits/web")

if not have or type(web) ~= "table" then web = nil end

local win, err = ui.window{ title = "Browser", w = W, h = H, x = 80, y = 60 }

if not win then
  print("browser: " .. tostring(err))
  return
end

--------------------------------------------------------------------------
-- An address, which is four numbers and a path.
--------------------------------------------------------------------------

local function split(text)
  local rest = tostring(text or ""):gsub("^%s+", ""):gsub("%s+$", "")
  local hostport, path = rest:match("^([^/]+)(/.*)$")

  hostport = hostport or rest
  path = path or "/"

  local host, port = hostport:match("^([^:]+):(%d+)$")

  host = host or hostport
  port = tonumber(port) or 80

  local a, b, c, d = tostring(host):match("^(%d+)%.(%d+)%.(%d+)%.(%d+)$")

  if not a then return nil end

  a, b, c, d = tonumber(a), tonumber(b), tonumber(c), tonumber(d)

  if a > 255 or b > 255 or c > 255 or d > 255 then return nil end

  return string.char(a, b, c, d), port, path
end

--------------------------------------------------------------------------
-- Text, wrapped to the window.
--
-- The document's text content arrives as one string with whatever spacing
-- the markup had in it, so runs of blanks collapse before anything is
-- measured. Wrapped by character count rather than by pixels because the
-- font is fixed width, which is the one simplification this can make
-- honestly.
--------------------------------------------------------------------------

local function wrap(text, width)
  local out = {}
  local space = gfx.measure(" ")

  for line in tostring(text or ""):gmatch("[^\n]+") do
    local current, taken = nil, 0

    --
    -- Greedy, word by word, and each word measured once.
    --
    -- Measuring the whole candidate line on every word would be quadratic
    -- in the length of a paragraph; adding one advance at a time is what a
    -- line breaker actually does, and it is the same shape the layout
    -- engine will need when it breaks a real inline box.
    --
    for word in line:gmatch("%S+") do
      local w = gfx.measure(word)

      if current == nil then
        current, taken = word, w
      elseif taken + space + w <= width then
        current = current .. " " .. word
        taken = taken + space + w
      else
        out[#out + 1] = current
        current, taken = word, w
      end
    end

    if current ~= nil then out[#out + 1] = current end
  end

  return out
end

--------------------------------------------------------------------------

local status = ui.label{ x = 12, y = H - 26, w = W - 24, text = "",
                         follow = { "left", "right", "bottom" } }

local page = ui.list{ x = 12, y = 44 + BAR_H, w = W - 24, h = H - 90 - BAR_H,
                      items = { "" },
                      follow = { "left", "right", "top", "bottom" } }

-- A page is read, not chosen from, so nothing is highlighted.
page.selected = 0

local history = {}

local address = ui.field{ x = 108, y = 10 + BAR_H, w = W - 200,
                          text = "10.0.2.2:8000/",
                          follow = { "left", "right", "top" } }

local function say(text)
  status.text = text
end

--
-- One load: connect, ask, read until the far end hangs up, parse.
--
-- The read loop is `fetch`'s, including the read *after* the loop: a close
-- and the last bytes can arrive in the same segment, and stopping at
-- `closed` would lose them.
--
local function load(text, remember)
  if web == nil then
    say("this image has no web kit - build it with `make WEB=1`")
    return
  end

  local where, port, path = split(text)

  if not where then
    say("that is not an address - four numbers, no names, there is no DNS")
    return
  end

  say("connecting...")

  local conn, why = fs.connect("/net", where, port)

  if not conn then
    local said = ({ [4] = "no route to it", [7] = "it refused the connection",
                    [9] = "it did not answer", [5] = "too many connections" })[why]

    say(said or ("could not connect: " .. tostring(why)))
    return
  end

  local request = ("GET %s HTTP/1.0\r\nHost: kosmos\r\nConnection: close\r\n\r\n")
                  :format(path)

  conn:write(request)

  local parts, total = {}, 0

  for _ = 1, 400 do
    local piece = conn:read()

    if piece then
      parts[#parts + 1] = piece
      total = total + #piece
    end

    if conn:closed() then break end

    conn:wait(25)
  end

  local last = conn:read()

  if last then
    parts[#parts + 1] = last
    total = total + #last
  end

  conn:close()

  local reply = table.concat(parts)
  local body = reply:match("\r\n\r\n(.*)$") or reply

  if total == 0 then
    say("nothing came back")
    return
  end

  local doc, bad = web.parse(body)

  if not doc then
    say("fetched " .. total .. " bytes, but it did not parse: " .. tostring(bad))
    return
  end

  local title = doc:title()

  --
  -- `retitle`, not `win.title = ...`. The title bar belongs to the desktop
  -- and is drawn by it, so changing the field here changes a local copy and
  -- nothing on screen - which is exactly what the first screenshot showed:
  -- a window called "Browser" displaying a page called something else.
  --
  win:retitle(title and ("Browser - " .. title) or "Browser")
  page.items = wrap(doc:text("body") or "", math.max(80, page.w - 28))
  page.top = 1
  page.selected = 0

  if #page.items == 0 then page.items = { "(the document has no text)" } end

  -- The *document*, not the response: `total` includes the HTTP headers,
  -- and reporting those as the page's size while showing only the body is
  -- a number that quietly does not match what is on screen.
  say(("%d bytes, %d paragraphs, %d links, %d headings - text only, "
       .. "there is no layout engine yet")
      :format(#body, doc:count("p"), doc:count("a"),
              doc:count("h1") + doc:count("h2") + doc:count("h3")))

  doc:close()

  if remember ~= false then history[#history + 1] = text end
end

--------------------------------------------------------------------------
-- The chrome.
--------------------------------------------------------------------------

win:add(ui.button{
  x = 12, y = 8 + BAR_H, w = 40, h = 24, text = "<",
  on_click = function()
    if #history < 2 then
      say("nothing to go back to")
      return
    end

    table.remove(history)                 -- where we are now
    local previous = history[#history]

    address.text = previous
    load(previous, false)
  end,
})

win:add(ui.button{ x = 58, y = 8 + BAR_H, w = 44, h = 24, text = "R",
                   on_click = function() load(address.text, false) end })
win:add(address)
win:add(ui.button{ x = W - 86, y = 8 + BAR_H, w = 74, h = 24, text = "Go",
                   follow = { "right", "top" },
                   on_click = function() load(address.text) end })
win:add(page)
win:add(status)

--
-- An address on the command line, which `wm browser:10.0.2.2:8000/` passes
-- through. Loaded before the first pass rather than after, so the window is
-- drawn once, with the page already in it.
--
local start = tostring(args or ""):match("^%s*(.-)%s*$")

if web == nil then
  page.items = { "This image was built without the web libraries.",
                 "", "  make WEB=1 qemu", "",
                 "builds one that has them." }
  say("no web kit in this image")
elseif start ~= "" then
  address.text = start
  load(start)
else
  say("an address is four numbers - there is no resolver yet")
end

win:run()
