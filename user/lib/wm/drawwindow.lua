-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- One window drawn, clipped: its frame, its tab and its pixels - a part of
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
  local OUT, back, theme, tabs, windows, strips =
    ctx.OUT, ctx.back, ctx.theme, ctx.tabs, ctx.windows, ctx.strips
  local frame_of, boxes_x, resizable = ctx.frame_of, ctx.boxes_x, ctx.resizable
  local focused_colour, idle_colour, title_colour =
    ctx.focused_colour, ctx.idle_colour, ctx.title_colour

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
        local tab = focused and focused_colour() or idle_colour()

        --
        -- Undecorated windows skip all of it: no tab, no border, no controls.
        --
        -- Menus have always escaped this by accident rather than by rule -
        -- they live in their own list and are composited by a different loop,
        -- so this one never sees one. The backdrop and the strip do not have
        -- that luck: they are ordinary entries in `windows`, and the strip
        -- came up wearing a title bar that said "Topbar" with a minimise box
        -- on the end of it.
        --
        -- `frame_of` already knows which windows these are - it is the
        -- function that says a menu, the backdrop and the strip are their own
        -- rectangle with nothing added. This asks it the same question a
        -- second way, and that is the part worth not repeating: one predicate,
        -- used by the thing that measures and by the thing that paints.
        --
        -- Undecorated: the backdrop, the strip, and a window that is the
        -- screen. The last used to be left out, so a tab, its title and its
        -- boxes were painted under a full-screen window's top rows on every
        -- pass, only for its contents to cover them.
        local bare = win.backdrop or win.strip or win.fullscreen

        -- The shadow is drawn before this, by `compose_rect`: it lies
        -- outside the frame, and `r` here is only the frame's visible part.

        --
        -- The corners, kept before anything is painted over them. Put back at
        -- the end of this window's drawing, which is what rounds it.
        --
        local kept = not bare
                     and OUT.corners(fx, fy, fw, fh, r) or nil

        if kept then OUT.keep(kept) end

        -- The whole decoration in one colour: the tab and the border all the
        -- way round, yellow when this window has the focus and grey when it
        -- does not.
        --
        -- **The tab is BeOS's again, by default**, and wide across the frame
        -- when Appearance says so (`tabs`). This comment used to record the
        -- full bar as a deliberate departure from BeOS - a tab as wide as its
        -- title bought nothing on a desktop that does not stack windows, and
        -- a narrow tab drew a handle smaller than the one the pointer took.
        -- Diego chose the tab on 18 September, and the second objection went
        -- with it: the pointer takes the tab's own shape now (`window_at`).
        --
        -- Clipped to the damage rectangle, which the contents below have
        -- always been and this had never been.
        --
        -- The comment above explains that the primitives clip to the
        -- backbuffer rather than to the rectangle being composed, and uses
        -- that to skip windows the rectangle does not touch. It did not
        -- finish the thought: a window the rectangle touches *at all* was
        -- having its whole frame filled. Ten pixels of damage on a 360x264
        -- window cost 95,040 of them.
        --
        -- Which is what the profile found. A dragged window composed half
        -- the pixels of an animating one and took four fifths of the time,
        -- and a cost that does not fall when the damage does is a cost that
        -- is not being charged to the damage.
        --
        local dx0 = (fx > r.x) and fx or r.x
        local dy0 = (fy > r.y) and fy or r.y
        local dx1 = math.min(fx + fw, r.x + r.w)
        local dy1 = math.min(fy + fh, r.y + r.h)

        if not bare then
          --
          -- The tab is a gradient and the border below it is not, which is
          -- why this is two fills where it was one.
          --
          -- The single fill above covered both, because both were the same
          -- colour: `tab` is the whole decoration, and the border says which
          -- window is listening by being the same yellow as the bar. A
          -- gradient run over all of it would shade the border too, and a
          -- border that is lighter at the top of a window than at the bottom
          -- reads as a lighting error rather than as a surface.
          --
          -- `fy` and `TAB_H` rather than `dy0` and the damaged height: the
          -- ramp belongs to the bar, not to whatever piece of it is being
          -- repainted. `theme.vgradient` says why at length.
          --
          local band = math.max(dy0, math.min(dy1, fy + OUT.TAB_H))

          -- Across the tab, which is the whole frame only in the full style:
          -- beside a BeOS tab is what is behind, and nothing is painted there.
          local tx1 = math.min(dx1, fx + tabs.width(win))

          if dy0 < band and tx1 > dx0 then
            local top, bottom = theme.chrome(tab)

            theme.vgradient(back, dx0, dy0, tx1 - dx0, band - dy0,
                            top, bottom, fy, OUT.TAB_H)
          end

          if band < dy1 then
            back:fill(dx0, band, dx1 - dx0, dy1 - band, tab)
          end
        end

        -- The close box, at the left of the tab where BeOS put it. A square
        -- outline rather than a cross: at this size a cross is four grey
        -- pixels and a smudge.
        --
        -- Both it and the title only when the rectangle reaches the tab at
        -- all. A window whose *contents* changed damages the area below the
        -- bar, and redrawing a title nobody disturbed is a string of glyphs
        -- per frame for nothing.
        if not bare and r.y < fy + OUT.TAB_H and r.y + r.h > fy then
          --
          -- Raised, like every other control in the system.
          --
          -- These were 8x8 flat squares painted straight onto the tab, from
          -- before `ui.md` 16.8b decided the look was dimensional. Next to a
          -- bevelled button they read as a smudge rather than as something
          -- you press - which is precisely the sentence a bevel is there to
          -- say, and the title bar is the one place every window has one.
          --
          -- The face is the window colour rather than the tab's, so a control
          -- looks like a control and not like a hole in the amber.
          --
          local by = fy + (OUT.TAB_H - OUT.BOX) // 2

          -- The title starts at the margin: nothing is to the left of it any
          -- more, since the close box moved to the right with the other two.
          --
          -- In the title font, which is its own role.
          --
          -- It used to be the widget font, and the two are not the same
          -- question: a title bar is a label on a piece of chrome and can
          -- afford a face with some character in it, where a widget font has
          -- to work at every size in every list in the system. Asking one
          -- setting to answer both meant choosing a display face for the
          -- title bars and getting it in every list as well.
          --
          -- `gfx.measure` with the same role, so the vertical centring is of
          -- the font that will actually be drawn.
          --
          back:text(fx + OUT.TITLE_IN,
                    fy + (OUT.TAB_H - gfx.height("title")) // 2,
                    win.title, title_colour(), tab, "title")

          --
          -- Maximise, minimise and close, all at the right and in the slots
          -- `OUT.SLOT` gives them - `boxes_x` says why the split went.
          local mx = boxes_x(win)

          if win.pinned then goto no_controls end

          local lit = (OUT.hover_boxes == win)

          -- Minimise, amber (`OUT.light`, `roadmap.md` 5zq).
          OUT.light(mx + OUT.BOX_W * OUT.SLOT.minimise, by, "minimise", lit)

          --
          -- Maximise: a little window - a frame with a title bar on it - and
          -- the bar is what makes it read as a window rather than as an
          -- empty box.
          --
          -- **Greyed, not gone, on a window that cannot be maximised** - one
          -- that draws its own pixels into a surface of a fixed size. It was
          -- left off, and the tab's controls then changed from one window to
          -- the next. Diego, 22 September: "when a window cant be maximixed we
          -- shouldnt remove the button we should just gray it out and disable
          -- it". Flat rather than raised, since raised is how this look says
          -- a thing can be pressed (`ui.md` 16.8b), and its glyph dimmed; a
          -- press on it does nothing.
          --
          -- Green, or grey and without a glyph when it cannot be used.
          local zx = mx + OUT.BOX_W * OUT.SLOT.maximise

          OUT.light(zx, by, "maximise", lit, not resizable(win))

          --
          -- Close, last and furthest right, which is where a hand that has
          -- used anything else goes. A square, the way BeOS drew it: a cross
          -- would need diagonals and there is no line primitive, so it would
          -- be fourteen one-pixel fills to say what a square says in two.
          --
          -- Red, outermost.
          local cx = mx + OUT.BOX_W * OUT.SLOT.close

          OUT.light(cx, by, "close", lit)

          ::no_controls::
        end

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
          if win.backdrop then
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
        -- The page rounded inside the frame, then **the corners back, last of
        -- all.** Everything this window drew is on the screen now, square;
        -- this copies what was behind over the pixels outside the arc and the
        -- window is round. One place, after every drawing call rather than
        -- inside any of them.
        --
        if kept then
          OUT.round_inside(win, r, tab)
          OUT.put_back(kept, fx, fy, fw, fh)
        end
      end
  end

  return draw_window
end
