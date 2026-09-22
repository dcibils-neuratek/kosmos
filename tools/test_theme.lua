-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The themes that ship, read on the host by the parser the desktop uses.
--
-- A theme names its faces since 22 September 2026 (`roadmap.md` 5s,
-- `docs/plex.html`): Diego asked for one called Plex that looks "exactly as
-- the mockup", and then "like a theme is a complete color scheme + font
-- selection?". What this holds, without booting anything:
--
--   every shipped theme reads with no complaint and names all five roles;
--   every face it names is a file in `assets/fonts/`, by the rule `gfx.c`'s
--   `font_asset` resolves names with - so a theme cannot name a face the
--   image does not carry and fall back to the bitmap in silence;
--   the four themes that were palettes name exactly the faces the system
--   shipped with, so choosing one changes nothing about its words;
--   Plex is the colours and faces `docs/plex.html` lists, value for value;
--   a line that is not a face and a size is told, and a role a file leaves
--   out comes from the defaults - which a theme setting a role never
--   changes, because the tables are copied;
--   and `theme.apply` puts a theme's colours in force and never its faces.
--
-- Run from the top of the tree by `make host-check`, which hands it the
-- font files: `build/host/lua tools/test_theme.lua assets/fonts/*.ttf`.

local theme = dofile("user/lib/theme.lua")
local themes = dofile("user/lib/themes.lua")

local checks, fails = 0, 0

local function check(ok, what)
  if ok then
    checks = checks + 1
  else
    fails = fails + 1
    print("  " .. what)
  end
end

-- `gfx.c`'s names: the file's stem, lower case, `-regular` dropped. The
-- files are the Makefile's `FONT_FILES`, handed over as arguments - the
-- list the image embeds, rather than a directory read a second way.
local names = {}

for _, file in ipairs(arg) do
  local stem = file:match("([^/]+)%.[ot]tf$")

  if stem then
    stem = stem:lower():gsub("%-regular$", "")
    names[#names + 1] = stem
  end
end

check(#names >= 10, "the image embeds " .. #names .. " faces; were "
      .. "FONT_FILES handed over?")

-- An exact name first, then a prefix - `font_asset`'s order.
local function resolves(want)
  for _, n in ipairs(names) do
    if n == want then return n end
  end

  for _, n in ipairs(names) do
    if n:sub(1, #want) == want and #want > 0 then return n end
  end

  return nil
end

-- 1. Every theme that ships: no complaint, five faces, each one carried.
for _, name in ipairs(themes.order) do
  local p, said = theme.read(themes[name], "dark")

  check(p and #said == 0,
        name .. " was not read cleanly: " .. table.concat(said or {}, "; "))
  check(p and p.name == name, name .. " calls itself " .. tostring(p and p.name))

  for _, role in ipairs(theme.roles) do
    local f = p and p.fonts and p.fonts[role]

    check(f and type(f.font) == "string" and math.type(f.px) == "integer",
          name .. " names no face for " .. role)

    if f then
      check(resolves(f.font) ~= nil,
            name .. "'s " .. role .. " is " .. tostring(f.font)
            .. ", which is no face in assets/fonts")
    end
  end
end

-- 2. The four that were palettes look as they always have - their faces,
-- and a Deskbar in the tab's colours, which is what it was painted in
-- before `bar` was a colour of its own.
for _, name in ipairs({ "photon", "beos", "platinum", "irix" }) do
  local p = theme.read(themes[name], "dark")

  check(p.bar == p.tab and p.bar_text == p.tab_text,
        name .. "'s Deskbar is not its tab's colours any more")

  -- And the spacing the kit always drew with.
  check(p.row_pad == 0 and p.button_pad_y == 5 and p.button_pad_x == 12
        and p.field_pad_y == 3 and p.field_pad_x == 4,
        name .. "'s spacing is not the kit's own: rows " .. tostring(p.row_pad)
        .. ", buttons " .. tostring(p.button_pad_y) .. "x"
        .. tostring(p.button_pad_x) .. ", fields " .. tostring(p.field_pad_y)
        .. "x" .. tostring(p.field_pad_x))

  for _, role in ipairs(theme.roles) do
    local want, got = theme.default_fonts[role], p.fonts[role]

    check(got.font == want.font and got.px == want.px,
          name .. "'s " .. role .. " is " .. got.font .. " " .. got.px
          .. ", not the " .. want.font .. " " .. want.px .. " it shipped with")
  end
end

-- 3. Plex, value for value against `docs/plex.html`.
do
  local p = theme.read(themes.plex, "dark")
  local colours = {
    desktop = 0xff3d63b8, window = 0xfff4f4f1, raised = 0xffe7e7e3,
    sunken = 0xffffffff, line = 0xff777777, line_soft = 0xffcfcfc9,
    edge_light = 0xffffffff, edge_dark = 0xff9a9a94, text = 0xff1e1e1e,
    text_dim = 0xff6c6c66, text_on = 0xffffffff, tab = 0xfff2c230,
    tab_idle = 0xffe7e7e3, tab_text = 0xff3a2e00, desktop_text = 0xffffffff,
    console = 0xff1c1c1e, console_text = 0xffececec, accent = 0xff2a55c9,
    good = 0xff2f8a3e, bad = 0xffb3261e, ring = 0xff2a55c9,
    stamp = 0xff8fa9df, bar = 0xffe7e7e3, bar_text = 0xff1e1e1e,
  }

  for k, v in pairs(colours) do
    check(p[k] == v, ("plex's %s is %08x, not %08x"):format(k, p[k] or 0, v))
  end

  local faces = {
    ui      = { "ibmplexsans", 16 },
    title   = { "ibmplexsanscondensed", 14 },
    heading = { "ibmplexsans-semibold", 15 },
    text    = { "ibmplexsans", 16 },
    mono    = { "ibmplexmono", 12 },
  }

  for role, f in pairs(faces) do
    local got = p.fonts[role]

    check(got.font == f[1] and got.px == f[2],
          "plex's " .. role .. " is " .. got.font .. " " .. got.px
          .. ", not " .. f[1] .. " " .. f[2])
  end

  -- And its spacing, `docs/plex.html`'s: rows 7, buttons 5 by 16, fields
  -- 5 by 8.
  check(p.row_pad == 7 and p.button_pad_y == 5 and p.button_pad_x == 16
        and p.field_pad_y == 5 and p.field_pad_x == 8,
        "plex's spacing is not rows 7, buttons 5x16, fields 5x8")

  -- Chosen for its weight: an exact face, not a prefix standing in for one.
  check(resolves("ibmplexsans-semibold") == "ibmplexsans-semibold",
        "Plex Sans SemiBold is not a face the image carries")
end

-- 4. What a file somebody writes can get wrong, and what it leaves out.
do
  local p, said = theme.read([[
name = mine
font.ui = ibmplexmono 11
font.nope = ibmplexsans 14
font.text = ibmplexsans
font.mono = ibmplexmono 400
]], "dark")

  check(p.fonts.ui.font == "ibmplexmono" and p.fonts.ui.px == 11,
        "a face and a size were not read")
  check(#said == 3, "three bad lines gave " .. #said .. " complaints: "
        .. table.concat(said, "; "))
  check((said[1] or ""):find("no role called `nope`", 1, true),
        "an unknown role was told as: " .. tostring(said[1]))
  check((said[2] or ""):find("not a face and a size", 1, true),
        "a face with no size was told as: " .. tostring(said[2]))
  check((said[3] or ""):find("400 pixels", 1, true),
        "a size of 400 was told as: " .. tostring(said[3]))

  local d = theme.default_fonts
  check(p.fonts.text.font == d.text.font and p.fonts.heading.px == d.heading.px,
        "a role the file left out did not come from the defaults")
  check(d.ui.font == "ibmplexsans" and d.ui.px == 16,
        "setting a theme's role changed the defaults to "
        .. d.ui.font .. " " .. d.ui.px)
end

-- 4b. Spacing a file gets wrong: each told, and the defaults kept.
do
  local p, said = theme.read([[
row_pad = -1
button_pad = 5
field_pad = 5 8 9
row_pad = 40
button_pad = 4 20
]], "dark")

  check(#said == 4, "four bad spacing lines gave " .. #said .. " complaints: "
        .. table.concat(said, "; "))
  check((said[1] or ""):find("whole number of pixels, 0 to 32", 1, true),
        "a negative row padding was told as: " .. tostring(said[1]))
  check((said[2] or ""):find("2 whole numbers", 1, true),
        "a button padding with one number was told as: " .. tostring(said[2]))
  check(p.row_pad == 0 and p.field_pad_x == 4,
        "a bad spacing line changed the spacing")
  check(p.button_pad_y == 4 and p.button_pad_x == 20,
        "a good button padding after bad lines was not read")
end

-- 4b'. The Deskbar's height: every theme that ships names today's 36, and
-- a bar too short for its 32-pixel icons is told, with the bounds.
do
  for _, name in ipairs(themes.order) do
    local p = theme.read(themes[name], "dark")

    check(p.bar_h == 36, name .. "'s Deskbar is " .. tostring(p.bar_h)
          .. " tall, not 36")
  end

  local p, said = theme.read("bar_h = 20\nbar_h = 44\n", "dark")

  check(#said == 1 and (said[1] or ""):find("36 to 64", 1, true),
        "a 20-pixel Deskbar was told as: " .. tostring(said[1]))
  check(p.bar_h == 44, "a 44-pixel Deskbar after a bad line was not read")
end

-- 4c. Words that read on a Deskbar somebody coloured (`theme.ink_on`):
-- dark on the light bars, white on the dark ones - every swatch Appearance
-- offers, by the answer it has to get.
do
  local dark, white = 0xff1e1e1e, 0xffffffff
  local want = {
    [0xffe7e7e3] = dark,  [0xffffcb00] = dark,  [0xfff2c230] = dark,
    [0xffcccccc] = dark,  [0xffa59f80] = dark,  [0xff5786da] = white,
    [0xff2b2b2b] = white, [0xff223344] = white, [0xff336698] = white,
  }

  for colour, ink in pairs(want) do
    check(theme.ink_on(colour) == ink,
          ("words on a %06x bar are %08x, not %08x"):format(colour & 0xffffff,
          theme.ink_on(colour), ink))
  end
end

-- 5. Applying a theme puts its colours in force and never its faces.
do
  local before = theme.fonts
  local ui_before = theme.fonts.ui.font .. " " .. theme.fonts.ui.px
  local p = theme.read(themes.plex, "dark")

  theme.apply(p)

  check(theme.desktop == 0xff3d63b8, "applying Plex did not paint its desktop")
  check(theme.row_pad == 7 and theme.field_pad_x == 8
        and theme.current().row_pad == 7 and theme.current().button_pad_x == 16,
        "applying Plex did not put its spacing in force, or current() does "
        .. "not carry it to other windows")
  check(theme.fonts == before
        and theme.fonts.ui.font .. " " .. theme.fonts.ui.px == ui_before,
        "applying Plex replaced the faces in force with its own, without "
        .. "loading one")
end

if fails == 0 then
  print(("PASS: %d checks on the themes that ship (every face carried, the "
         .. "four palettes unchanged, Plex as docs/plex.html has it, a bad "
         .. "line told, and a palette applied without its faces)."):format(checks))
  os.exit(0)
end

print(("FAIL: %d of %d checks on the themes."):format(fails, checks + fails))
os.exit(1)
