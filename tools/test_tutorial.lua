-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Cafesa3D's tutorial held to Cafesa3D and to Kosmos's browser, on this
-- computer with no machine booted (`docs/cafesa3d-tutorial/`, `roadmap.md`
-- 4l).
--
-- A tutorial is prose about a program, and prose has nobody to tell it when
-- the program moves: a button renamed, a tab taken away, and the page goes
-- on sending a beginner to look for it. So this reads both and holds one to
-- the other, the way `make test` holds the code:
--
--   * Cafesa3D opens the tutorial at its first page, which the image
--     carries;
--   * every page and picture in the folder is reachable from that page, and
--     every link and picture a page names is in the folder - the image
--     carries the whole folder, so a stray file would ship and a missing
--     one would be a hole;
--   * every picture is one `gfx.png` decodes - eight bits a channel, RGB or
--     RGBA, not interlaced - at the size its page gives it, so the browser
--     shows it pixel for pixel, and carries the licence line in a `tEXt`
--     chunk, as `tools/cafesa3d_tutorial_shots.py` writes it;
--   * every page uses only what Kosmos's browser draws - a table would be
--     dropped without a word - and no symbol its fonts may not have;
--   * every control a page names in bold - `<b class="ui">Base colour</b>`
--     - is a name the application's source has in quotes.
--
--   build/host/lua tools/test_tutorial.lua docs/cafesa3d-tutorial/*

local APP = "user/bin/apps/cafesa3d.lua"
local PREFIX = "tutorial/cafesa3d/"

local checks, failed = 0, 0

local function check(ok, what)
  checks = checks + 1

  if not ok then
    failed = failed + 1
    print("not ok - " .. what)
  end
end

local function read(path)
  local f = io.open(path, "rb")

  if not f then return nil end

  local text = f:read("a")

  f:close()
  return text
end

local app = read(APP)

-- The folder, as the Makefile hands it over: this Lua has no `popen`.
local dir, files = nil, {}

for _, path in ipairs(arg) do
  dir = dir or path:match("^(.*/)")
  files[path:match("([^/]+)$")] = true
end

check(dir ~= nil, "given no files: build/host/lua tools/test_tutorial.lua docs/cafesa3d-tutorial/*")
check(files["index.html"], "the folder has no index.html")

local index = app:match('local TUTORIAL = { index = "asset:([^"]+)" }')

check(index == PREFIX .. "index.html",
      ("Cafesa3D opens the tutorial at %s, not at its first page"):format(tostring(index)))

-- What Kosmos's browser draws: the elements `web_style.c` gives a block
-- or an inline style, and `web_paint.c`'s pictures, less the ones these
-- pages have no use for.
local TAGS = { html = true, head = true, meta = true, title = true, style = true, body = true,
               h1 = true, h2 = true, h3 = true, p = true, ul = true, li = true, a = true,
               b = true, strong = true, i = true, em = true, code = true, kbd = true,
               img = true }

-- Every quoted string in the application, folded: a tab is "material" in
-- the source and Material on the screen.
local said = {}

for s in app:gmatch('"([^"\n]+)"') do said[s:lower()] = true end

-- A PNG's size, and whether `gfx.png` takes it (`user/kits/gfx/png.c`).
local function png(bytes)
  if not bytes or bytes:sub(1, 8) ~= "\x89PNG\r\n\26\n" or bytes:sub(13, 16) ~= "IHDR" then
    return nil, "not a PNG"
  end

  local w, h, depth, colour, _, _, interlace = string.unpack(">I4I4BBBBB", bytes, 17)

  if depth ~= 8 or (colour ~= 2 and colour ~= 6) or interlace ~= 0 then
    return nil, ("depth %d, colour type %d, interlace %d, which gfx.png refuses")
                :format(depth, colour, interlace)
  end

  return w, h
end

-- From the first page, every page and picture it leads to.
local reached, queue = { ["index.html"] = true }, { "index.html" }
local pages = 0

while #queue > 0 do
  local name = table.remove(queue, 1)
  local text = read(dir .. name) or ""

  pages = pages + 1

  check(text:match("^<!DOCTYPE html>\n<!%-%- Kosmos%. Copyright") ~= nil,
        name .. " does not carry the licence line")
  check(text:match("<title>[^<]+</title>") ~= nil, name .. " has no title")

  for tag in text:gmatch("<(%a%w*)") do
    check(TAGS[tag:lower()], ("%s uses <%s>, which the browser does not draw"):format(name, tag))
  end

  -- Kosmos's fonts are Latin: the vertical dots of the dots menu came out
  -- of the browser as a question mark.
  check(not text:find("&#8%d%d%d;"),
        name .. " spells a symbol by number, which Kosmos's fonts may not have")

  for href in text:gmatch('href="([^"]+)"') do
    check(files[href], ("%s links to %s, which is not in the folder"):format(name, href))

    if files[href] and not reached[href] then
      reached[href] = true
      queue[#queue + 1] = href
    end
  end

  for img in text:gmatch("<img[^>]*>") do
    local src = img:match('src="([^"]+)"')
    local w, h = tonumber(img:match('width="(%d+)"')), tonumber(img:match('height="(%d+)"'))

    check(src and files[src], ("%s shows %s, which is not in the folder"):format(name,
          tostring(src)))
    check(img:match('alt="[^"]+"'), ("%s shows %s with no words for it"):format(name,
          tostring(src)))

    if src and files[src] then
      reached[src] = true

      local bytes = read(dir .. src)
      local pw, ph = png(bytes)

      check(pw ~= nil, ("%s: %s"):format(src, tostring(ph)))
      check(bytes and bytes:sub(1, 512):find("Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.",
                                             1, true),
            src .. " does not carry the licence line in a text chunk")
      check(pw == nil or (w == pw and h == ph),
            ("%s gives %s a box of %sx%s, and it is %sx%s"):format(name, src, tostring(w),
             tostring(h), tostring(pw), tostring(ph)))
    end
  end

  local named = 0

  for label in text:gmatch('<b class="ui">([^<]+)</b>') do
    named = named + 1
    check(said[label:lower()], ('%s names "%s", which %s never says'):format(name, label, APP))
  end

  if name ~= "index.html" and name ~= "9-keys.html" then
    check(named > 0, name .. " names no control at all")
  end
end

local pictures = 0

for file in pairs(files) do
  check(reached[file], file .. " is in the folder and nothing leads to it")

  if file:match("%.png$") then pictures = pictures + 1 end
end

print(("tutorial: %d checks, %d failed, over %d pages and %d pictures"):format(checks, failed,
      pages, pictures))
os.exit(failed == 0 and 0 or 1)
