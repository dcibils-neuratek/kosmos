-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Compositing: each damaged rectangle drawn from the back of the stack to
-- the front, and put on the screen - a part of `wm.lua` in a file of its
-- own (`roadmap.md` 6zn). The two functions as they stood there, made by a
-- function handed what they read of the rest; the damage list is emptied
-- in place at the end of a pass, since `add_damage` holds the same one.
--

return function(ctx)
  local OUT, OUTLINE, P, PT =
    ctx.OUT, ctx.OUTLINE, ctx.P, ctx.PT
  local back, damage, draw_cursor, draw_desktop =
    ctx.back, ctx.damage, ctx.draw_cursor, ctx.draw_desktop
  local draw_window, focused_colour, frame_of, menus =
    ctx.draw_window, ctx.focused_colour, ctx.frame_of, ctx.menus
  local osd, screen, subtract_into, tabs =
    ctx.osd, ctx.screen, ctx.subtract_into, ctx.tabs
  local windows =
    ctx.windows

  local function compose_rect(r)
    --
    -- What each window still shows, and what is left for the desktop.
    --
    local visible  = {}
    local remaining = { r }

    for i = #windows, 1, -1 do
      if #remaining == 0 then break end

      local win = windows[i]

      if not win.hidden then
        --
        -- A decorated window is its tab and its body (`tabs.shape`), each cut
        -- out of what is behind in turn; everything else is its rectangle.
        -- With the tab across the whole frame the two meet exactly, and the
        -- cut is the rectangle it always was.
        --
        local shape, round

        if win.kind == "menu" or win.backdrop or win.strip
           or win.fullscreen then
          shape = { { frame_of(win) } }
        else
          shape = { tabs.shape(win) }
          round = OUT.corner_squares(win)
        end

        local mine = nil

        for _, part in ipairs(shape) do
          local fx, fy, fw, fh = part[1], part[2], part[3], part[4]
          local keep = {}

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
                mine = { x0 = x0, y0 = y0, x1 = x1, y1 = y1 }
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
              if win.backdrop then
                keep[#keep + 1] = piece
              else
                subtract_into(keep, piece, x0, y0, x1, y1)

                if round then OUT.uncover(keep, round, x0, y0, x1, y1) end
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
    for i = 1, #windows do
      local v = visible[i]

      OUT.cast_shadow(windows[i], r)

      if v then
        if P.measuring then
          P.prof.drawn = P.prof.drawn + (v.x1 - v.x0) * (v.y1 - v.y0)
        end

        draw_window(i, { x = v.x0, y = v.y0, w = v.x1 - v.x0, h = v.y1 - v.y0 })
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
