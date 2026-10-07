-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Compositing: each damaged rectangle drawn from the back of the stack to
-- the front, and put on the screen - a part of `wm.lua` in a file of its
-- own (`roadmap.md` 6zn). The two functions as they stood there, made by a
-- function handed what they read of the rest; the damage list is emptied
-- in place at the end of a pass, since `add_damage` holds the same one.
--

--
-- **Nothing made a pass** (7 October, `testing.md` 18.439): composing was
-- more than half of all the window manager allocated - 2.1 KB a pass on the
-- M700 - in small tables for every damaged rectangle and every window: the
-- pieces left to draw, each window's shape, its visible box, the rectangle
-- handed to `draw_window`. Those are scratch, dead before the rectangle is
-- done, so they come from pools here, reset for each damaged rectangle, and
-- the lists alternate between two kept for the purpose. The collector is
-- the jitter (`frames`), and the cheapest pause is the one never caused.
--
local rects, nrect = {}, 0

local function rect(x, y, w, h)
  nrect = nrect + 1

  local t = rects[nrect]

  if not t then t = {} rects[nrect] = t end

  t.x, t.y, t.w, t.h = x, y, w, h
  return t
end

local boxes, nbox = {}, 0

local function box(x0, y0, x1, y1)
  nbox = nbox + 1

  local t = boxes[nbox]

  if not t then t = {} boxes[nbox] = t end

  t.x0, t.y0, t.x1, t.y1 = x0, y0, x1, y1
  return t
end

-- A list emptied, to be filled again, without making a new one.
local function clear(t)
  for i = #t, 1, -1 do t[i] = nil end
  return t
end

-- The lists a rectangle's pieces pass between, what each window shows, and
-- a window's shape: one part or two, four numbers each.
local list_a, list_b, visible, parts = {}, {}, {}, {}

--
-- `a` with `b` cut out of it, appended to `out` as up to four rectangles.
--
-- The pieces are taken in bands - above, below, then left and right of what
-- is left - so they never overlap. Overlapping pieces would be drawn twice,
-- which is what this whole exercise exists to stop.
--
local function subtract_into(out, a, bx0, by0, bx1, by1)
  local ax1, ay1 = a.x + a.w, a.y + a.h

  if by0 > a.y then
    out[#out + 1] = rect(a.x, a.y, a.w, by0 - a.y)
  end

  if by1 < ay1 then
    out[#out + 1] = rect(a.x, by1, a.w, ay1 - by1)
  end

  local y0 = (by0 > a.y) and by0 or a.y
  local y1 = (by1 < ay1) and by1 or ay1

  if y1 > y0 then
    if bx0 > a.x then
      out[#out + 1] = rect(a.x, y0, bx0 - a.x, y1 - y0)
    end

    if bx1 < ax1 then
      out[#out + 1] = rect(bx1, y0, ax1 - bx1, y1 - y0)
    end
  end
end

return function(ctx)
  local OUT, OUTLINE, P, PT =
    ctx.OUT, ctx.OUTLINE, ctx.P, ctx.PT
  local back, damage, draw_cursor, draw_desktop =
    ctx.back, ctx.damage, ctx.draw_cursor, ctx.draw_desktop
  local mirror = ctx.mirror
  local draw_window, focused_colour, frame_of, menus =
    ctx.draw_window, ctx.focused_colour, ctx.frame_of, ctx.menus
  local osd, screen, tabs =
    ctx.osd, ctx.screen, ctx.tabs
  local windows =
    ctx.windows

  --
  -- Everything that is visible in one damage rectangle, and nothing that is
  -- not.
  --
  -- **This used to be a painter's algorithm with nothing taken out of it.**
  -- The comment it replaces said so plainly - "windows are opaque, so there
  -- is no blending between them and the order is the whole of the occlusion"
  -- - and the order *is* enough to make the picture right. It is not enough
  -- to make it cheap: a window completely behind another was blitted in full
  -- and then painted over, and so was the wallpaper underneath both.
  --
  -- What that costs is not theoretical. Six Doom windows and four cubes, all
  -- animating and heavily overlapped: the compositor took 17% of four
  -- processors while each Doom took one or two. The compositor's cost scales
  -- with window *area*, not with how hard anything is working, so it grows
  -- fastest exactly when the machine is busiest - and most of that area was
  -- pixels nobody would ever see.
  --
  -- So: two passes. The first walks **front to back** and works out which
  -- pieces of the rectangle each window actually shows, cutting away what the
  -- windows above it cover. The second draws **back to front**, as before.
  --
  -- The order of the two matters and is not interchangeable. Culling has to
  -- be front to back, because occlusion accumulates downwards. Drawing has to
  -- be back to front, because not every primitive here clips to the rectangle
  -- it was given - the title text does not - and back to front is what makes
  -- that harmless: whatever a lower window paints outside its piece, a higher
  -- one paints over. Drawing front to back with the same culling would be
  -- faster still and would need every primitive audited first.
  --
  -- Each window is drawn once, clipped to the *bounding box* of its visible
  -- pieces rather than once per piece. A window split into an L is then still
  -- redrawing a little of what is hidden, which is the cheap ninety per cent
  -- of this: the expensive case is a window that is entirely hidden, and that
  -- one is skipped outright.
  --
  local function compose_rect(r)
    --
    -- What each window still shows, and what is left for the desktop.
    --
    nrect, nbox = 0, 0

    -- By window number, with gaps: emptied by its keys, not its length.
    for k in pairs(visible) do visible[k] = nil end

    local remaining = clear(list_a)

    remaining[1] = r

    -- In the order they are drawn: the stack, and a dock in front of all of
    -- it (`OUT.order`, nil when there is no dock).
    local order = OUT.order()
    local count = order and #order or #windows

    for k = count, 1, -1 do
      if #remaining == 0 then break end

      local i = order and order[k] or k
      local win = windows[i]

      if not win.hidden then
        --
        -- A decorated window is its tab and its body (`tabs.shape`), each cut
        -- out of what is behind in turn; everything else is its rectangle.
        -- With the tab across the whole frame the two meet exactly, and the
        -- cut is the rectangle it always was.
        --
        local nparts, round

        if win.kind == "menu" or win.backdrop or win.strip
           or win.fullscreen or win.tip then
          parts[1], parts[2], parts[3], parts[4] = frame_of(win)
          nparts = 1
        elseif win.headed or win.popup then
          -- No tab: its rectangle, and rounded (`roadmap.md` 6zj) - a
          -- popup as well, which is a page with nothing round it.
          parts[1], parts[2], parts[3], parts[4] = frame_of(win)
          nparts = 1
          round = OUT.corner_squares(win)
        else
          nparts = tabs.shape(win, parts)
          round = OUT.corner_squares(win)
        end

        local mine = nil

        for part = 0, nparts - 1 do
          local fx, fy = parts[part * 4 + 1], parts[part * 4 + 2]
          local fw, fh = parts[part * 4 + 3], parts[part * 4 + 4]
          local keep = clear((remaining == list_a) and list_b or list_a)

          for _, piece in ipairs(remaining) do
            local x0 = (fx > piece.x) and fx or piece.x
            local y0 = (fy > piece.y) and fy or piece.y
            local x1 = math.min(fx + fw, piece.x + piece.w)
            local y1 = math.min(fy + fh, piece.y + piece.h)

            if x1 > x0 and y1 > y0 then
              if mine then
                if x0 < mine.x0 then mine.x0 = x0 end
                if y0 < mine.y0 then mine.y0 = y0 end
                if x1 > mine.x1 then mine.x1 = x1 end
                if y1 > mine.y1 then mine.y1 = y1 end
              else
                mine = box(x0, y0, x1, y1)
              end

              --
              -- **The backdrop hides nothing.** It is transparent wherever it
              -- has not drawn an icon, so what the compositor paints under it -
              -- the wallpaper, or the flat colour, and the stamp - still has to
              -- be painted. Every other window is opaque and cuts away what is
              -- behind it, which is what this pass is for.
              --
              -- Without this the desktop was not merely covering the wallpaper:
              -- a window that covers a rectangle makes `draw_desktop` skip it
              -- altogether, so the picture was never drawn at all.
              --
              -- And a strip that asked to be blended, which is as
              -- transparent as the backdrop wherever it has drawn nothing.
              if win.backdrop or win.blend then
                keep[#keep + 1] = piece
              else
                subtract_into(keep, piece, x0, y0, x1, y1)

                if round then OUT.uncover(keep, round, x0, y0, x1, y1, rect) end
              end
            else
              keep[#keep + 1] = piece
            end
          end

          remaining = keep
        end

        visible[i] = mine
      end
    end

    -- The desktop, only where no window reaches. Often nowhere.
    for _, piece in ipairs(remaining) do
      if P.measuring then P.prof.drawn = P.prof.drawn + piece.w * piece.h end
      draw_desktop(piece)
    end

    -- And the windows, bottom to top, each clipped to what it shows - each
    -- with its shadow first, which lies outside what it shows.
    for k = 1, count do
      local i = order and order[k] or k
      local v = visible[i]

      OUT.cast_shadow(windows[i], r)

      if v then
        if P.measuring then
          P.prof.drawn = P.prof.drawn + (v.x1 - v.x0) * (v.y1 - v.y0)
        end

        draw_window(i, rect(v.x0, v.y0, v.x1 - v.x0, v.y1 - v.y0))
      end
    end

    --
    -- Menus, above every window and below the cursor.
    --
    -- No decoration and no tab: a menu is its rectangle. One groove around
    -- it so it reads as sitting on top of what is behind it rather than
    -- being part of it - which is the whole job of the border on a thing
    -- that floats.
    --
    -- **Rounded as a window is, by keeping its corners and putting them
    -- back** (`OUT.corners`). Everything behind a menu has been composed by
    -- now, so what is kept is exactly what should show outside its curve.
    -- Menus were square here while the kit drew a rounded line inside them,
    -- and a flat look's menu showed its surface's dark ground outside that
    -- line once the kit stopped filling a control's square (0.10.152).
    --
    for i = 1, #menus do
      local m = menus[i]

      if m.x < r.x + r.w and m.x + m.w > r.x
         and m.y < r.y + r.h and m.y + m.h > r.y then
        local x0 = (m.x > r.x) and m.x or r.x
        local y0 = (m.y > r.y) and m.y or r.y
        local x1 = math.min(m.x + m.w, r.x + r.w)
        local y1 = math.min(m.y + m.h, r.y + r.h)

        if x1 > x0 and y1 > y0 then
          local kept = OUT.corners(m.x, m.y, m.w, m.h, r)

          OUT.keep(kept)
          back:blit(m.surface, x0 - m.x, y0 - m.y,
                    x1 - x0, y1 - y0, x0, y0)
          OUT.put_back(kept, m.x, m.y, m.w, m.h)
        end
      end
    end

    --
    -- The rubber band, above every window: this is where the frame is going.
    -- In the focused window's colour, because it is that window being resized
    -- and the eye should not have to work that out.
    --
    if PT.outline then
      back:fill(PT.outline.x, PT.outline.y, PT.outline.w, OUTLINE, focused_colour())
      back:fill(PT.outline.x, PT.outline.y + PT.outline.h - OUTLINE, PT.outline.w,
                OUTLINE, focused_colour())
      back:fill(PT.outline.x, PT.outline.y, OUTLINE, PT.outline.h, focused_colour())
      back:fill(PT.outline.x + PT.outline.w - OUTLINE, PT.outline.y, OUTLINE,
                PT.outline.h, focused_colour())
    end

    -- The level bar, over every window and under the pointer.
    if osd.shown then
      local a = osd.alpha(sys.ticks())
      local x0, y0 = math.max(r.x, osd.x), math.max(r.y, osd.y)
      local x1 = math.min(r.x + r.w, osd.x + osd.W)
      local y1 = math.min(r.y + r.h, osd.y + osd.H)

      if a > 0 and x1 > x0 and y1 > y0 then
        back:blend(osd.surface, x0 - osd.x, y0 - osd.y, x1 - x0, y1 - y0,
                   x0, y0, a)
      end
    end

    -- Last, so it is on top of everything, and before the blit, so what
    -- reaches the screen is a frame with a cursor in it.
    draw_cursor()

    screen:blit(back, r.x, r.y, r.w, r.h, r.x, r.y)

    -- And said: on virtio-gpu nothing drawn is shown until it is sent, and
    -- on ramfb this is a call that does nothing (`roadmap.md` 4h a).
    screen:flush(r.x, r.y, r.w, r.h)

    -- And to whoever watches the screen (`vncd`): the same rectangle, the
    -- same frame, cursor included.
    mirror(r)
  end

  local function compose()
    if #damage == 0 then return end

    if P.measuring then
      P.prof.frames = P.prof.frames + 1
      P.prof.rects  = P.prof.rects + #damage
    end

    for _, r in ipairs(damage) do
      --
      -- **`px` is what the compositor was asked for; `drawn` is what it did.**
      --
      -- They used to be one number, and that number was this one - the area
      -- of the damage rectangle - which cannot see occlusion culling by
      -- construction. It reported 170714 pixels a frame before culling and
      -- 170395 after, identical to the noise, because the damage did not
      -- change: what changed was how much of it got painted more than once.
      --
      -- A metric that answers a different question than the one being asked
      -- is worse than no metric, because it is read as an answer. So both are
      -- reported now, and the ratio between them is the interesting figure:
      -- overdraw.
      --
      if P.measuring then P.prof.px = P.prof.px + r.w * r.h end
      compose_rect(r)
    end

    -- Emptied where it is rather than replaced: `add_damage` in `wm.lua`
    -- holds this same list, and a new one here would be a list only this
    -- file had (`roadmap.md` 6zn).
    for i = #damage, 1, -1 do damage[i] = nil end
  end

  return compose
end
