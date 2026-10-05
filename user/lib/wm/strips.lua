-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The window manager's menu bar over a window that draws its own pixels, a
-- part of `wm.lua` in a file of its own (`roadmap.md` 6zn). The section as
-- it stood there, in a function handed what it reads of the rest: the
-- backbuffer, the look, `post` and `add_damage` - none of them ever replaced
-- while the window manager runs, so each is kept as it is handed (`post` as
-- a function that finds it, since its body comes later in `wm.lua`).
--
-- A menu bar above a window that draws its own pixels.
--
-- Diego chose this on 18 September for the Super Nintendo's File menu (the
-- README's decision log): a direct window's contents are the application's
-- own memory, so the kit cannot draw a menu bar into them - `window:paint`
-- returns at once for one - and the alternative was each application
-- painting an imitation of one into its game. So the window manager draws
-- the strip, as the kit draws `ui.menubar`: the same gradient and groove,
-- the same titles in the same places. The application's buffer is the area
-- below it, and everything it is told about the pointer is in the buffer's
-- own coordinates.
--
-- **Only the strip is here.** A press on a title is posted as a `menubar`
-- event with where the menu should open, and the application opens an
-- ordinary kit menu - a window, `kind = "menu"`, owned by its window - so
-- the menus themselves look and behave exactly like every other one, and
-- Doom and Quake can have them the same way.
--

return function(ctx)
  local back, theme = ctx.back, ctx.theme
  local post, add_damage = ctx.post, ctx.add_damage

  local strips = {}

  -- A glyph and eight pixels of air: `ui.menubar`'s height.
  function strips.height()
    return gfx.height("ui") + 8
  end

  -- Where each title starts and ends: `ui.menubar`'s `spans`, the same sums.
  function strips.spans(titles)
    local out, x = {}, 4

    for i, t in ipairs(titles) do
      local w = gfx.measure(t) + 16

      out[i] = { x = x, w = w }
      x = x + w
    end

    return out
  end

  function strips.paint(win)
    local mb = win.menubar
    local s, w, h = mb.surface, win.w, mb.h
    local top, bottom = theme.chrome(theme.raised)

    theme.vgradient(s, 0, 0, w, h - 2, top, bottom, 0, h - 2)
    s:fill(0, h - 2, w, 1, theme.edge_dark)
    s:fill(0, h - 1, w, 1, theme.edge_light)

    for i, sp in ipairs(strips.spans(mb.titles)) do
      s:text(sp.x + 8, (h - 2 - gfx.height("ui")) // 2, mb.titles[i],
             theme.text)
    end

    add_damage(win.x, win.y, w, h)
  end

  --
  -- Asked for, and checked: a list of at most eight titles, each a string of
  -- at most thirty-two characters, on a window that draws its own pixels. A
  -- window manager is a server, and a server takes what it expects.
  --
  function strips.accept(win, titles)
    if not win.shared or type(titles) ~= "table" then return end

    local kept = {}

    for i = 1, math.min(#titles, 8) do
      if type(titles[i]) == "string" then
        kept[#kept + 1] = titles[i]:sub(1, 32)
      end
    end

    if #kept == 0 then return end

    local h = strips.height()

    win.menubar = { titles = kept, h = h,
                    surface = gfx.surface{ w = win.w, h = h } }
    win.h = win.h + h
    strips.paint(win)
  end

  -- How far below the window's top the application's own pixels begin.
  function strips.below(win)
    return win.menubar and win.menubar.h or 0
  end

  -- A press on the strip: a title opens its menu under it; between titles,
  -- nothing. Said, as a window's placing is, so the log shows which menu of
  -- which window was opened and where - a harness finds the menu by it.
  function strips.press(win, nx)
    local mb = win.menubar

    for i, sp in ipairs(strips.spans(mb.titles)) do
      if nx >= win.x + sp.x and nx < win.x + sp.x + sp.w then
        print(("wm: menu bar %s of %s at %d,%d"):format(mb.titles[i],
              win.title, win.x + sp.x, win.y + mb.h))
        post(win, { type = "menubar", index = i, title = mb.titles[i],
                    x = win.x + sp.x, y = win.y + mb.h })
        return
      end
    end
  end

  -- The strip from its own surface, and the application's pixels below it.
  function strips.compose(win, from, x0, y0, x1, y1)
    local split = win.y + win.menubar.h

    if y0 < split then
      local yb = math.min(y1, split)

      back:blit(win.menubar.surface, x0 - win.x, y0 - win.y,
                x1 - x0, yb - y0, x0, y0)
    end

    if y1 > split then
      local ya = math.max(y0, split)

      local lower = win.h - win.menubar.h

      if win.src_w and (win.src_w ~= win.w or win.src_h ~= lower) then
        back:stretch(from, 0, 0, win.src_w, win.src_h,
                     win.x, split, win.w, lower, nil, false,
                     x0, ya, x1 - x0, y1 - ya)
      else
        back:blit(from, x0 - win.x, ya - split, x1 - x0, y1 - ya, x0, ya)
      end
    end
  end

  return strips
end
