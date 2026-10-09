-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- One window drawn, clipped: its frame and its pixels - a part of
-- `wm.lua` in a file of its own (`roadmap.md` 6zn). The function as it
-- stood there, made by a function handed what it reads of the rest. Every
-- one of those is a table or a function that is never replaced, so each is
-- kept as handed; the window's measurements are `OUT`'s, read each time,
-- because a change of scale sets them again.
--
--
-- One window, clipped to `r`.
--
-- Lifted out of `compose_rect` unchanged - the parameter is named `r` for
-- exactly that reason, so that two hundred lines of drawing and the
-- reasoning attached to it did not have to be re-read to be moved.
--

return function(ctx)
  local OUT, back, theme, windows, strips =
    ctx.OUT, ctx.back, ctx.theme, ctx.windows, ctx.strips
  local frame_of, resizable = ctx.frame_of, ctx.resizable

  local function draw_window(i, r)
      local win = windows[i]
      local focused = (i == #windows)
      local fx, fy, fw, fh = frame_of(win)

      --
      -- Windows that this rectangle does not touch are skipped, and the ones
      -- it does touch are drawn only where it touches them.
      --
      -- This is what makes dragging cost the same with eight windows open as
      -- with one. Without it every damage rectangle redrew every window in
      -- full - the primitives clip to the *backbuffer*, not to the rectangle
      -- being composed - so moving one window re-blitted the entire desktop
      -- twice per step, once for where it was and once for where it now is.
      -- What that feels like is a drag that gets heavier as you open things,
      -- which is exactly what it was.
      --
      if not win.hidden
         and fx < r.x + r.w and fx + fw > r.x
         and fy < r.y + r.h and fy + fh > r.y then

        --
        -- **No window has a tab or a border the window manager draws** since
        -- one chrome's step 3 (8 October): a window is its page. An ordinary
        -- one is rounded and has the three drawn over its header below; the
        -- backdrop, the strip, a full-screen window and a tip are square.
        --
        local rounded = not (win.backdrop or win.strip or win.fullscreen or win.tip)

        -- The shadow is drawn before this, by `compose_rect`: it lies
        -- outside the frame, and `r` here is only the frame's visible part.

        --
        -- The corners, kept before anything is painted over them. Put back at
        -- the end of this window's drawing, which is what rounds it.
        --
        local kept = rounded
                     and OUT.corners(fx, fy, fw, fh, r) or nil

        if kept then OUT.keep(kept) end

        -- And the contents, clipped to the intersection. The window's own
        -- surface is the source, so the source rectangle moves with the clip:
        -- reading from 0,0 and drawing at the clipped position would slide
        -- the picture inside its own frame.
        local x0 = (win.x > r.x) and win.x or r.x
        local y0 = (win.y > r.y) and win.y or r.y
        local x1 = math.min(win.x + win.w, r.x + r.w)
        local y1 = math.min(win.y + win.h, r.y + r.h)

        if x1 > x0 and y1 > y0 then
          -- Whichever buffer the application is not drawing into, or the
          -- surface this process owns for an ordinary window.
          local from = win.surface

          if win.shared then
            from = win.shared[win.shared.live]
          end

          --
          -- Blended for the backdrop and copied for everything else. The
          -- desktop is transparent between its icons and the wallpaper is
          -- underneath it; `blend` is source-over and costs more than a copy,
          -- which is why it is not what every window gets.
          --
          if win.backdrop or win.blend then
            back:blend(from, x0 - win.x, y0 - win.y,
                       x1 - x0, y1 - y0, x0, y0)
          elseif win.menubar then
            strips.compose(win, from, x0, y0, x1, y1)
          elseif win.src_w and (win.src_w ~= win.w or win.src_h ~= win.h) then
            --
            -- A surface in the application's points, at a scale: stretched
            -- to its place, and only this damaged piece of it written.
            --
            back:stretch(from, 0, 0, win.src_w, win.src_h,
                         win.x, win.y, win.w, win.h, nil, false,
                         x0, y0, x1 - x0, y1 - y0)
          else
            back:blit(from, x0 - win.x, y0 - win.y,
                      x1 - x0, y1 - y0, x0, y0)
          end
        end

        --
        -- The sizing grip, over the window's own bottom-right corner.
        --
        -- Three diagonal steps rather than a solid block, which is the
        -- shape every desktop uses for this and is readable at a glance
        -- without a label. Drawn after the contents so it sits on top of
        -- them, and only on windows that can actually be resized - see
        -- `resizable`.
        --
        if resizable(win) then
          local gx = win.x + win.w - OUT.GRIP
          local gy = win.y + win.h - OUT.GRIP

          for step = 0, 2 do
            local o = step * 5
            local n = OUT.GRIP - 3 - o

            if n > 0 then
              -- Light above, dark below: the same two edges every raised
              -- thing here is made of, at a diagonal.
              back:fill(gx + o + 2, gy + OUT.GRIP - 3 - o, n, 1, theme.edge_light)
              back:fill(gx + o + 2, gy + OUT.GRIP - 2 - o, n, 1, theme.edge_dark)
            end
          end
        end

        --
        -- **The three, over the header that left room for them** (`handlers.
        -- lights`): in colour on the window in front and grey on the rest,
        -- which is how a window without a tab says it has the focus - the
        -- tab's colour said it before. Their glyphs while the pointer is
        -- over them, as on a tab; maximise greyed on a window that cannot
        -- be resized. Only where the rectangle being drawn reaches them.
        --
        if win.headed and not win.pinned then
          local bx, by, bw, bh = OUT.boxes_rect(win)

          if bx and bx < r.x + r.w and bx + bw > r.x
             and by < r.y + r.h and by + bh > r.y then
            local lit = (OUT.hover_boxes == win)

            local held = OUT.held_box and OUT.held_box.win == win and OUT.held_box.slot

            for kind, slot in pairs(OUT.SLOT) do
              OUT.light(bx + OUT.BOX_W * slot, by, kind, lit or held == kind,
                        not focused or (kind == "maximise" and not resizable(win)),
                        held == kind)
            end
          end
        end

        --
        -- The page rounded inside the frame, then **the corners back, last of
        -- all.** Everything this window drew is on the screen now, square;
        -- this copies what was behind over the pixels outside the arc and the
        -- window is round. One place, after every drawing call rather than
        -- inside any of them. A window with no frame has no page inside one
        -- to round.
        --
        if kept then
          OUT.put_back(kept, fx, fy, fw, fh)
        end
      end
  end

  return draw_window
end
