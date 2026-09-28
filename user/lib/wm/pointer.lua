-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The pointer's pass: a press, a drag, a resize, a release - a part of
-- `wm.lua` in a file of its own (`roadmap.md` 6zn), the part 6zj's
-- windows without title bars will change most. The function as it stood
-- there, made by a function handed what it reads of the rest: the
-- pointer's state is `PT`'s and the window's measurements `OUT`'s, read
-- each time; everything else is a table or a function never replaced.
--

return function(ctx)
  local PT, OUT, W, H =
    ctx.PT, ctx.OUT, ctx.W, ctx.H
  local add_damage, boxes_x, by_handle, cursor_size =
    ctx.add_damage, ctx.boxes_x, ctx.by_handle, ctx.cursor_size
  local damage_outline, dismiss_menus, focused_window, frame_of =
    ctx.damage_outline, ctx.dismiss_menus, ctx.focused_window, ctx.frame_of
  local maximise, menu_at, menus, minimise =
    ctx.maximise, ctx.menu_at, ctx.menus, ctx.minimise
  local move_window, pointer_log, post, raise =
    ctx.move_window, ctx.pointer_log, ctx.post, ctx.raise
  local resizable, resize_window, scale, strips =
    ctx.resizable, ctx.resize_window, ctx.scale, ctx.strips
  local window_at =
    ctx.window_at

  --
  -- **A press on one of the three**, on a tab or over a header that is the
  -- title bar: `mx` is where the first starts. True when it landed on one,
  -- and false for the rest of a tab, which is a drag.
  --
  local function press_box(win, nx, mx)
    local slot = nx >= mx and OUT.IN_SLOT[(nx - mx) // OUT.BOX_W]

    if slot == "minimise" then
      minimise(win)
    elseif slot == "maximise" then
      -- Greyed on a window that cannot be maximised, and then a press
      -- on it is nothing: not a maximise, and not the start of a drag.
      if resizable(win) then maximise(win) end
    elseif nx >= mx + OUT.BOX_W * OUT.SLOT.close then
      --
      -- The close box. Asked first, taken by force second.
      --
      -- The window is told, and a window that is listening tidies up
      -- and goes. One that is not listening - the whole point of this
      -- desktop being able to survive one - never answers, so the
      -- request is remembered and collected on a later pass.
      --
      win.closing = sys.ticks() + OUT.close_grace
      post(win, { type = "close" })
    else
      return false
    end

    return true
  end

  local function pointer_pass(p)
    if not p then return end

    local range_x = (p.max_x - p.min_x)
    local range_y = (p.max_y - p.min_y)

    if range_x <= 0 or range_y <= 0 then return end

    local nx = (p.x - p.min_x) * (W - 1) // range_x
    local ny = (p.y - p.min_y) * (H - 1) // range_y

    local was_down = (PT.buttons & 1) ~= 0
    local is_down = (p.buttons & 1) ~= 0

    -- Both ends of a click, bounded; the driver logs the same two moments as
    -- `i8042 buttons`. If the driver logs a release and this does not, it was
    -- lost between the two. If both do and the widget stays down, it was lost
    -- after this - which on the first real machine it was, in an event queue
    -- `post` now empties more carefully.
    if is_down ~= was_down and pointer_log.said < 30 then
      pointer_log.said = pointer_log.said + 1
      print(("wm: button %s at %d,%d raw=%s"):format(
            is_down and "down" or "up", nx, ny, tostring(p.buttons)))
    end

    local moved_this_pass = (nx ~= PT.x or ny ~= PT.y)

    if moved_this_pass then
      -- Both rectangles: where it was, so it is erased, and where it is going,
      -- so it is drawn. Both are composited, so neither is ever half-done.
      --
      -- The badge a drag carries is drawn beside the arrow, so while one is up
      -- the rectangle has to cover both or the label smears across the screen.
      local cw, ch = cursor_size()

      add_damage(PT.x, PT.y, cw, ch)
      PT.x, PT.y = nx, ny
      add_damage(PT.x, PT.y, cw, ch)

      -- The title bar's three show their glyphs while the pointer is over
      -- them: repaint the three it left and the three it reached.
      local over = OUT.boxes_under(nx, ny)

      if over ~= OUT.hover_boxes then
        OUT.damage_boxes(OUT.hover_boxes)
        OUT.hover_boxes = over
        OUT.damage_boxes(over)
      end
    end

    --
    -- **The wheel, to the window under the pointer** (`roadmap.md` 5zv) - not
    -- the one with the focus, as every desktop has it: a list is scrolled by
    -- pointing at it, and pointing is all the wheel asks. In the window's own
    -- coordinates, as a press is, so the kit can find the view beneath; and
    -- posted, never sent, like everything this pass hands on. Nothing while
    -- a menu is open, which has no wheel to turn, and nothing over a title
    -- bar, which has nothing to scroll.
    --
    if (p.wheel or 0) ~= 0 and #menus == 0 then
      local win = OUT.wheel_target(nx, ny)

      if win then
        post(win, { type = "wheel", n = p.wheel,
                    x = nx - win.x, y = ny - win.y - strips.below(win) })
      end
    end

    --------------------------------------------------------------------------
    -- What a press means depends on where it lands.
    --
    --   the title bar    the window manager's: raise and drag
    --   the contents     the application's: forwarded, in that window's own
    --                    coordinates
    --
    -- Forwarded and not delivered, like a key: it goes on the window's queue
    -- and the application collects it whenever it gets round to asking. This
    -- process never calls an application, which is the whole reason a hung one
    -- cannot freeze the desktop - and a click is not an exception to that.
    --
    -- A press grabs. Everything until the release goes to the window the press
    -- landed in, even after the pointer has left it, because that is what lets
    -- a button un-press when you slide off it and a drag keep working past the
    -- edge. Without a grab, releasing outside would deliver the release to
    -- whatever happened to be underneath.
    --------------------------------------------------------------------------
    if is_down and not was_down then
      --
      -- Menus first, and they take the press whatever is under them.
      --
      -- A press inside one goes to the *owner's* queue tagged with the menu's
      -- handle, so an application polls one window and gets everything - which
      -- is the reason a menu belongs to a window rather than standing alone.
      --
      -- A press outside every menu dismisses them and stops there. It does not
      -- also reach whatever is underneath, which is what every desktop does
      -- and is the right answer: the first click after opening a menu is how
      -- you change your mind, not how you press the thing behind it.
      --
      if #menus > 0 then
        local m = menu_at(nx, ny)

        if m then
          post(by_handle[m.owner],
               { type = "mouse", menu = m.handle, action = "press",
                 x = nx - m.x, y = ny - m.y })
          PT.grabbed = m
        else
          dismiss_menus()
        end

        PT.buttons = p.buttons
        return
      end

      local win, fx, fy = window_at(nx, ny)

      if win then
        raise(win)

        --
        -- Super and Control held: the window moves, from wherever it was
        -- pressed, and the application never sees the press - a button under
        -- the pointer is not pressed, a game's picture is not clicked. Not the
        -- desktop, the Deskbar or a full-screen window, which do not move.
        --
        if OUT.chord.move_held() and not (win.backdrop or win.strip or win.fullscreen) then
          PT.dragging = { win = win, dx = nx - win.x, dy = ny - win.y,
                       held = true }
          OUT.chord.super_moved = true

        --
        -- The backdrop and the strip are not windows you move.
        --
        -- They have no title bar, so there is nothing that *looks* like a
        -- handle - but the hit test asks where the pointer is relative to the
        -- frame, and for an undecorated window the frame starts at its own
        -- first row. So the top eighteen pixels of the menu bar dragged it,
        -- and the desktop could be picked up by its top edge and slid off the
        -- screen with every icon on it.
        --
        -- Not a special case so much as the same rule as the decoration: a
        -- window with no tab has no tab to grab.
        --
        -- **And a full-screen window has none either**, which this left out
        -- when full screen arrived: its top rows were taken for a title bar
        -- that is not drawn, so a press there dragged it, and one at the top
        -- right - where the close box would be - asked it to close. Found on
        -- 26 September when Cafesa3D's dots, at exactly that corner, closed
        -- Cafesa3D instead of opening its menu.
        --
        elseif win.backdrop or win.strip or win.fullscreen then
          -- Straight to the application, which is what a bar is for - and
          -- grabbed, like any other press, or the release never arrives and a
          -- shortcut is a word that highlights and does nothing.
          PT.grabbed = win
          post(win, { type = "mouse", action = "press",
                      x = nx - win.x, y = ny - win.y })
        elseif not win.headed and ny < fy + OUT.TAB_H then
          local mx = boxes_x(win)

          if win.pinned then
            -- Nothing on this tab but the tab. Drag it and that is all.
            PT.dragging = { win = win, dx = nx - win.x, dy = ny - win.y }
          elseif not press_box(win, nx, mx) then
            PT.dragging = { win = win, dx = nx - win.x, dy = ny - win.y }
          end
        elseif win.headed and not win.pinned and OUT.boxes_under(nx, ny) == win then
          --
          -- **The three over a header that is the title bar** (`roadmap.md`
          -- 6zj): the same three and the same presses as on a tab. What is
          -- beside them is the window's own, and a press there goes to it -
          -- the header hands a press on its empty band back as a drag
          -- (`handlers.move_begin`).
          --
          press_box(win, nx, boxes_x(win))
        elseif win.menubar and ny < win.y + win.menubar.h then
          strips.press(win, nx)
        elseif resizable(win)
               and nx >= win.x + win.w - OUT.GRIP and nx < win.x + win.w
               and ny >= win.y + win.h - OUT.GRIP and ny < win.y + win.h then
          --
          -- The grip, and it is tested before the contents on purpose: this
          -- square belongs to the window manager, and an application that
          -- happens to have drawn something there does not get the press.
          --
          PT.resizing = { win = win, ox = nx, oy = ny, ow = win.w, oh = win.h,
                       w = win.w, h = win.h }

          PT.outline = { frame_of(win) }
          PT.outline = { x = PT.outline[1], y = PT.outline[2],
                      w = PT.outline[3], h = PT.outline[4] }
          damage_outline(PT.outline)
        else
          PT.grabbed = win
          post(win, { type = "mouse", action = "press",
                      x = nx - win.x, y = ny - win.y - strips.below(win) })
        end
      end
    elseif not is_down and was_down then
      if PT.grabbed then
        if PT.grabbed.kind == "menu" then
          post(by_handle[PT.grabbed.owner],
               { type = "mouse", menu = PT.grabbed.handle, action = "release",
                 x = nx - PT.grabbed.x, y = ny - PT.grabbed.y })
        else
          post(PT.grabbed, { type = "mouse", action = "release",
                          x = nx - PT.grabbed.x,
                          y = ny - PT.grabbed.y - strips.below(PT.grabbed) })
        end

        PT.grabbed = nil
      end

      --
      -- And where the drag ended, which is the one thing only this process
      -- knows: the release above went to the window the press started in,
      -- wherever the pointer had got to since.
      --
      -- The release first and the drop second, on purpose. The source's own
      -- view has to come out of its drag before it is told about one - and
      -- when a drop lands back in the window it came from, which is how you
      -- move a file into a subfolder without opening it, those two are the
      -- same window.
      --
      if PT.drag then
        local onto = window_at(nx, ny)

        add_damage(PT.x, PT.y, cursor_size())
        damage_outline(PT.outline)
        PT.outline = nil

        if onto and onto.drops then
          post(onto, { type = "drop", kind = PT.drag.kind, payload = PT.drag.payload,
                       x = nx - onto.x, y = ny - onto.y })

          -- One reply, from that window, until the next drop. `handlers.dropped`
          -- is the only thing that reads this.
          PT.answering = { from = PT.drag.from, to = onto.handle }
        else
          -- Nowhere that takes it. The source is told so rather than left
          -- waiting for an answer that is not coming.
          post(by_handle[PT.drag.from],
               { type = "dropped", ok = false, count = 0,
                 error = "nothing there takes a drop" })
        end

        PT.drag = nil
      end

      --
      -- The one resize, on release. Everything up to here was an outline.
      --
      if PT.resizing then
        damage_outline(PT.outline)
        PT.outline = nil

        resize_window(PT.resizing.win, PT.resizing.w, PT.resizing.h)
        PT.resizing = nil
      end

      -- The end of a drag is where the application is told, once, rather
      -- than on every step of it.
      if PT.dragging then
        post(PT.dragging.win, { type = "moved",
                             x = PT.dragging.win.x, y = PT.dragging.win.y })

        -- Said for a move by the keys, which nothing else shows but pixels,
        -- as a keyboard's move is said below.
        if PT.dragging.held then
          print(("wm: moved %s by Super + Control to %d,%d"):format(
                tostring(PT.dragging.win.title), PT.dragging.win.x, PT.dragging.win.y))
        elseif PT.dragging.header then
          -- And by a header, whose drag began in the window rather than
          -- here (`handlers.move_begin`).
          print(("wm: moved %s by its header to %d,%d"):format(
                tostring(PT.dragging.win.title), PT.dragging.win.x, PT.dragging.win.y))
        end

        PT.dragging = nil
      end
    end

    if PT.resizing and is_down and moved_this_pass then
      --
      -- The band follows the pointer; the window does not move at all until
      -- the button comes up. Clamped here rather than in `resize_window`, so
      -- what the band shows is what you will get - an outline that promises a
      -- size the window will refuse is worse than no outline.
      --
      local win = PT.resizing.win
      local w = PT.resizing.ow + (nx - PT.resizing.ox)
      local h = PT.resizing.oh + (ny - PT.resizing.oy)

      local most_w, most_h = OUT.room(win)

      if w < scale.MIN_W then w = scale.MIN_W end
      if h < scale.MIN_H then h = scale.MIN_H end
      if w > most_w then w = most_w end
      if h > most_h then h = most_h end

      PT.resizing.w, PT.resizing.h = w, h

      damage_outline(PT.outline)

      -- The frame it will have: the page alone, for a window with no tab.
      if win.headed then
        PT.outline = { x = win.x, y = win.y, w = w, h = h }
      else
        PT.outline = { x = win.x - OUT.BORDER, y = win.y - OUT.TAB_H,
                    w = w + OUT.BORDER * 2, h = h + OUT.TAB_H + OUT.BORDER }
      end

      damage_outline(PT.outline)
    end

    --
    -- What would take it, outlined, while something is being carried.
    --
    -- The same rectangle a resize draws and for the same reason: four thin
    -- edges cost nothing, where highlighting the *area* would recomposite
    -- every window underneath on every pointer movement. The two can never be
    -- up at once - a press is either on the grip or in the contents - so they
    -- share the one variable rather than agreeing about which is on top.
    --
    if PT.drag and moved_this_pass then
      local onto = window_at(nx, ny)
      local want = nil

      if onto and onto.drops then
        local fx, fy, fw, fh = frame_of(onto)

        want = { x = fx, y = fy, w = fw, h = fh }
      end

      local same = PT.outline and want
                   and PT.outline.x == want.x and PT.outline.y == want.y
                   and PT.outline.w == want.w and PT.outline.h == want.h

      if not same then
        damage_outline(PT.outline)
        PT.outline = want
        damage_outline(PT.outline)
      end
    end

    if PT.dragging and is_down then
      move_window(PT.dragging.win, nx - PT.dragging.dx, ny - PT.dragging.dy, true)
    end

    --
    -- Movement, only while something is held.
    --
    -- Hover is not sent, and that is a decision rather than an omission. Every
    -- movement would be a message, an application would poll a queue full of
    -- them, and the whole path from here to a widget would run at the rate the
    -- pointer moves rather than at the rate anything changes. What a button
    -- needs to un-press when you slide off it is drag, and this is drag.
    --
    --
    -- Hover, but only while a menu is open.
    --
    -- Movement is otherwise sent only to whatever is holding a button - see
    -- the note below, which is still right: every movement would be a message
    -- and an application would poll a queue full of them.
    --
    -- A menu is the one case that genuinely needs the pointer without a
    -- button. You click a title, let go, and slide down the items; a submenu
    -- opens because the pointer passed over its parent, not because anything
    -- was pressed. Without this the whole menu is only usable by dragging.
    --
    -- Bounded by the thing that makes it affordable: it happens only while
    -- menus are open, which is a second at a time and never while anything is
    -- trying to be fast.
    --
    if #menus > 0 and not is_down and moved_this_pass then
      local m = menu_at(nx, ny)

      if m then
        post(by_handle[m.owner],
             { type = "mouse", menu = m.handle, action = "move",
               x = nx - m.x, y = ny - m.y })
      end
    end

    if PT.grabbed and is_down and moved_this_pass and PT.grabbed.kind == "menu" then
      post(by_handle[PT.grabbed.owner],
           { type = "mouse", menu = PT.grabbed.handle, action = "move",
             x = nx - PT.grabbed.x, y = ny - PT.grabbed.y })
    elseif PT.grabbed and is_down and moved_this_pass then
      post(PT.grabbed, { type = "mouse", action = "move",
                      x = nx - PT.grabbed.x,
                      y = ny - PT.grabbed.y - strips.below(PT.grabbed) })
    elseif not is_down and moved_this_pass and #menus == 0 then
      -- A window that asked (`handlers.track`), and only while it has focus.
      local f = focused_window()

      if f and f.tracking then
        post(f, { type = "mouse", action = "move", hover = true,
                  x = nx - f.x, y = ny - f.y - strips.below(f) })
      end
    end

    --------------------------------------------------------------------------
    -- And the right button, which does none of this process's own jobs.
    --
    -- No raise, no drag, no close box, no grip. Those are all answers to "you
    -- pressed the decoration", and a right press means "tell me about what is
    -- under the pointer" - a question about the *contents*, which is the
    -- application's to answer and not this one's. So the frame is not tested
    -- at all: a right press outside the contents does nothing.
    --
    -- Only `win.context` windows hear it - see `handlers.open` for why that is
    -- off by default - and it carries `button = "right"` so a window that
    -- asked for both can tell them apart. **No `button` field means the left
    -- one**, which is what every event this process has ever posted was, so
    -- nothing already written has to change to keep being right.
    --
    -- Grabbed like a left press, so the release reaches the same window even
    -- if the pointer has left it by then.
    --------------------------------------------------------------------------
    local was_right = (PT.buttons & 2) ~= 0
    local is_right  = (p.buttons & 2) ~= 0

    if is_right and not was_right then
      if #menus > 0 then
        --
        -- Into the menu, tagged with its handle, exactly as a left press is.
        --
        -- A menu row is the thing most worth asking about - "what is this and
        -- what would it start" - and the Deskbar's rows are launchers, which
        -- are files somebody may want to edit. Sending this to the owner is
        -- what lets it answer without this process learning what a menu row
        -- means.
        --
        -- Outside every menu it dismisses them and stops, which is what a
        -- left press does and for the same reason: the first click after
        -- opening a menu is how you change your mind, not how you press the
        -- thing behind it.
        --
        local m = menu_at(nx, ny)

        if m then
          post(by_handle[m.owner],
               { type = "mouse", menu = m.handle, action = "press",
                 button = "right", x = nx - m.x, y = ny - m.y })
        else
          dismiss_menus()
        end
      else
        local win = window_at(nx, ny)

        if win and win.context
           and nx >= win.x and nx < win.x + win.w
           and ny >= win.y and ny < win.y + win.h then
          PT.right_grabbed = win
          post(win, { type = "mouse", action = "press", button = "right",
                      x = nx - win.x, y = ny - win.y })
        end
      end
    elseif not is_right and was_right and PT.right_grabbed then
      post(PT.right_grabbed, { type = "mouse", action = "release",
                            button = "right",
                            x = nx - PT.right_grabbed.x,
                            y = ny - PT.right_grabbed.y })
      PT.right_grabbed = nil
    end

    PT.buttons = p.buttons
  end

  return pointer_pass
end
