-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- Drawing commands onto a surface.
--
-- A window that sends drawing rather than owning pixels sends a list of
-- commands - `fill`, `text`, `tint` and the rest - which `ui.lua`'s drawing
-- context makes, and the window manager draws them into the window's
-- surface. These are those drawings, one function a command.
--
-- **In a library since 27 September** (`roadmap.md` 6n, step 6): they were
-- a table inside `wm.lua`, and a program that draws its own pixels could not
-- draw a kit widget - so Cafesa3D could not hold the IDE's editor in its
-- Script panel. Now the window manager and `ui.paint_view` replay the same
-- commands through the same functions, and a widget looks the same in
-- either kind of window.
--
-- `paint.new(picture, sized)` makes the table: `picture(name)` is a decoded
-- picture or nil, and `sized(role, px)` the face for a role at a size - each
-- process has its own of both. `paint.run(surface, ops, table)` draws a list.
--

local paint = {}

--
-- `always`: every piece of text through `sized`, not only one at a size of
-- its own - a window drawing itself at a scale, whose roles' faces are at
-- its points for laying out and whose text is drawn at the scale's pixels
-- (`docs/astra-display.md` D2c).
--
function paint.new(picture_named, sized, always)
  return {
    fill = function(s, o)
      s:fill(o.x or 0, o.y or 0, o.w or 0, o.h or 0, o.color or 0xff000000)
    end,

    --
    -- **A rounded rectangle, filled or outlined**, for the controls a flat
    -- look draws (`ui.lua`, `gc:raised`). Blended at the arc, so a button
    -- sits on whatever is behind it without knowing what that is.
    --
    -- `r` is the radius and is clamped by the primitive to half the shorter
    -- side, so a control too small to round is simply square rather than
    -- being drawn wrong.
    --
    fill_round = function(s, o)
      s:fill_round(o.x or 0, o.y or 0, o.w or 0, o.h or 0,
                   o.color or 0xff000000, o.r or 0)
    end,

    frame_round = function(s, o)
      s:frame_round(o.x or 0, o.y or 0, o.w or 0, o.h or 0,
                    o.color or 0xff000000, o.r or 0)
    end,

    --
    -- **A triangle**, which is the one shape a rectangle cannot stand in for.
    --
    -- An application drawing through commands had `fill`, `text` and `image`,
    -- and `triangle` was a surface method - reachable only by a window that
    -- owns its own pixels. So Music's play arrow was a staircase of thin
    -- fills, visibly stepped at 18 pixels, while the primitive sat in `gfx.c`
    -- unreachable. Diego chose the command over generating seven pictures, and
    -- the restyle after Music wants the same shape for menus, sliders and
    -- disclosure arrows.
    --
    triangle = function(s, o)
      s:triangle(o.x1 or 0, o.y1 or 0, o.x2 or 0, o.y2 or 0,
                 o.x3 or 0, o.y3 or 0, o.color or 0xffffffff)
    end,

    text = function(s, o)
      -- `o.role` picks the face. Absent, `gfx` uses the interface font, which
      -- is what every application that does not care wants. `o.px` asks for
      -- that role's font at another size, which is resolved here rather than
      -- sent as a number: a face index means nothing in this process. And
      -- `o.variant` a weight or a slant of it - Text Editor's bold.
      s:text(o.x or 0, o.y or 0, tostring(o.s or ""),
             o.color or 0xffffffff, o.bg,
             (always or o.px or o.variant) and sized(o.role, o.px, o.variant) or o.role)
    end,

    --
    -- Removed, because it could only ever have failed.
    --
    -- This called `s:blend(x, y, w, h, colour)` as though it were an alpha
    -- fill. `l_blend` in `gfx.c` is not one: it is a surface-to-surface alpha
    -- *blit* and wants a surface at argument two, so this passed a number
    -- where a userdata was checked for and raised every time.
    --
    -- It never raised, because nothing could reach it: `ui.lua`'s graphics
    -- context has no `blend` verb, so no application has ever sent the op.
    -- Dead and wrong at the same time, which is the pair that survives
    -- longest - neither the compiler nor the tests have anything to say about
    -- code nobody calls.
    --
    -- An alpha fill is genuinely wanted for a dimmed control or a wash behind
    -- a menu, and when it arrives it is new C in `gfx.c` plus a verb in the
    -- kit, not this line.
    --
    blend = function(s, o)
      local _ = s, o
    end,

    --
    -- A picture, named rather than carried.
    --
    -- Every other command carries what it draws. This one cannot: a decoded
    -- image is megabytes and a message is two kilobytes, so a surface can no
    -- more travel in one than a window's contents can.
    --
    -- So the application sends a *name* and this process loads it. That is
    -- not a workaround, it is the same rule as everywhere else here - the
    -- compositor owns the pixels, which is why a hung application still has a
    -- window - applied to pictures. An application says what it wants drawn;
    -- it never holds what is drawn.
    --
    -- Decoded once, on first use, and kept. A photograph is four megabytes
    -- and several hundred milliseconds of inflate; doing that per frame would
    -- be a slideshow.
    --
    image = function(s, o)
      local picture = picture_named(tostring(o.asset))

      if not picture then return end

      --
      -- `alpha` picks the compositing rule, and a picture needs both.
      --
      -- A photograph is opaque and wants `blit`, which is a `memcpy` a row at
      -- a time. An icon is not: these are RGBA and the corners outside the
      -- shape are transparent, so copying them would paint a grey square
      -- around every file - and worse, would paint over the selection colour
      -- underneath it.
      --
      -- `blend` is source-over with the source's own alpha, in C, and it is
      -- perhaps three times the work per pixel. Choosing per command rather
      -- than always blending is the difference between the two cases costing
      -- what they need and a full-screen photograph paying an icon's price.
      --
      --
      -- **And `dw`/`dh` mean draw it at that size**, which is what `stretch`
      -- is for. Without them this is a crop: the command says which part of
      -- the picture, and that part lands pixel for pixel. A cover inside an
      -- MP3 is five hundred pixels and Music draws it at 78 and at 44, so a
      -- crop would show the corner of a sleeve twice.
      --
      -- The primitive arrived first and nothing could reach it: an
      -- application draws through commands, and no command carried a size.
      --
      -- `smooth` averages what each pixel covers rather than taking the
      -- nearest (`gfx.c`'s `stretch`): an icon drawn smaller than it is, where
      -- nearest neighbour drops whole rows of its outline.
      --
      local dw = tonumber(o.dw) or 0
      local dh = tonumber(o.dh) or 0

      --
      -- **`fade`, all of it at once**, 0 to 255 over the picture's own
      -- alpha: the Deskbar's button breathing while its program starts
      -- (`docs/launching.html`). Clamped here, because `blend` and `stretch`
      -- refuse anything outside the range with an error, and this is drawing
      -- a command another process sent.
      --
      local fade = math.tointeger(tonumber(o.fade) or 255) or 255

      fade = math.max(0, math.min(255, fade))

      if dw > 0 and dh > 0 then
        s:stretch(picture, o.sx or 0, o.sy or 0, o.w or 0, o.h or 0,
                  o.x or 0, o.y or 0, dw, dh, o.alpha and fade or nil,
                  o.smooth == true)
      elseif o.alpha then
        s:blend(picture, o.sx or 0, o.sy or 0, o.w or 0, o.h or 0,
                o.x or 0, o.y or 0, fade)
      else
        s:blit(picture, o.sx or 0, o.sy or 0, o.w or 0, o.h or 0,
               o.x or 0, o.y or 0)
      end
    end,

    --
    -- **A picture used as a mask**, painted in the command's colour: the
    -- mockups' line icons (`gc:line_icon`, `gfx.c`'s `tint`). The picture
    -- is white with its coverage as alpha, so the colour is the caller's and
    -- the look's rather than the file's.
    --
    tint = function(s, o)
      local picture = picture_named(tostring(o.asset))

      if not picture then return end

      s:tint(picture, 0, 0, o.w or 0, o.h or 0, o.x or 0, o.y or 0,
             o.color or 0xff000000)
    end,
  }
end

-- A list of commands into a surface, in order; one it does not know is
-- skipped, as the window manager always skipped it.
-- Points to pixels, to the nearest: the window manager's `scale.px`.
local function px(v, pct)
  return math.floor(v * pct / 100 + 0.5)
end

--
-- **A drawing command, in the screen's pixels at a scale** (`ui.md` 16.18).
-- A rectangle by its two edges, so neighbours still meet at a step that is
-- not whole; text at its place, since its face is already loaded at the
-- scale (`apply_fonts`, `sized`); a triangle's corners, which `gfx` takes
-- as doubles; and a picture drawn at its scaled size, averaged - an icon
-- from its 64-pixel export when there is one, which is every icon Haiku's
-- set gives, so it is shrunk rather than blown up.
--
function paint.scale(o, pct, picture_named)
  local kind = o.op

  if kind == "fill" or kind == "fill_round" or kind == "frame_round" then
    local x, y = tonumber(o.x) or 0, tonumber(o.y) or 0
    local w, h = tonumber(o.w) or 0, tonumber(o.h) or 0
    local x0, y0 = px(x, pct), px(y, pct)

    o.x, o.y = x0, y0
    o.w, o.h = px(x + w, pct) - x0, px(y + h, pct) - y0

    -- The radius is a length like any other, so a control at 150 per cent
    -- is rounded a half more rather than keeping the pixels of a smaller
    -- screen.
    if o.r then o.r = px(o.r, pct) end
  elseif kind == "text" then
    o.x = px(tonumber(o.x) or 0, pct)
    o.y = px(tonumber(o.y) or 0, pct)
  elseif kind == "triangle" then
    for _, k in ipairs({ "x1", "y1", "x2", "y2", "x3", "y3" }) do
      o[k] = (tonumber(o[k]) or 0) * pct / 100
    end
  elseif kind == "tint" then
    --
    -- **A line icon at another size is another picture**, not this one
    -- resampled: `tools/lineicons.py` renders each from its vectors at 15,
    -- 19, 23 and 30, which is what 15 points comes to at 100, 125, 150 and
    -- 200 per cent. A one-pixel line averaged into a larger box is a grey
    -- smear; drawn again at the size, it is a line. A size with no picture
    -- keeps the 15 at the scaled place, which is small rather than wrong.
    --
    local x, y = tonumber(o.x) or 0, tonumber(o.y) or 0
    local want = px(tonumber(o.w) or 0, pct)
    local base = tostring(o.asset or ""):match("^(.-)%-%d+%.png$")

    o.x, o.y = px(x, pct), px(y, pct)

    if base and picture_named(("%s-%d.png"):format(base, want)) then
      o.asset = ("%s-%d.png"):format(base, want)
      o.w, o.h = want, want
    end
  elseif kind == "image" then
    local x, y = tonumber(o.x) or 0, tonumber(o.y) or 0
    local dw, dh = tonumber(o.dw) or 0, tonumber(o.dh) or 0

    -- No size given is a crop drawn pixel for pixel: its size is the crop's.
    if dw <= 0 or dh <= 0 then
      dw, dh = tonumber(o.w) or 0, tonumber(o.h) or 0
    end

    local x0, y0 = px(x, pct), px(y, pct)

    o.x, o.y = x0, y0
    o.dw, o.dh = px(x + dw, pct) - x0, px(y + dh, pct) - y0
    o.smooth = true

    -- An icon's 64: `16x16/<name>` or a bare 32 name, when the 64 exists.
    local asset = tostring(o.asset or "")
    local name, from = asset:match("^16x16/(.+)$"), 16

    if not name and not asset:find("/", 1, true) then
      name, from = asset, 32
    end

    if name and picture_named("64x64/" .. name) then
      local k = 64 // from

      o.asset = "64x64/" .. name
      o.sx, o.sy = (tonumber(o.sx) or 0) * k, (tonumber(o.sy) or 0) * k
      o.w, o.h = (tonumber(o.w) or 0) * k, (tonumber(o.h) or 0) * k
    end
  end
end

function paint.run(surface, ops, table_)
  for _, o in ipairs(ops or {}) do
    local fn = table_[o.op]

    if fn then fn(surface, o) end
  end
end

return paint
