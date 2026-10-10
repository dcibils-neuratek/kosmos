-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The app switcher: Super+Tab (`docs/switcher.html`, agreed as drawn on 10
-- October 2026: "All mockups are perfect") - a part of `wm.lua` in a file
-- of its own, as `osd.lua` is.
--
-- Super+Tab, held, shows every running application as a large icon in a
-- panel in the middle of the screen, in the order they were last used, the
-- one that will come forward lit and named under its icon; Tab moves on and
-- Shift+Tab or the left arrow back, wrapping; the down arrow opens a row of
-- the lit application's windows, each a small picture of what it holds, and
-- left and right choose one; Esc puts it away changing nothing; Q quits the
-- lit application; and letting go of Super brings the lit application's
-- windows forward - or the one window chosen. The pointer lights an icon,
-- and a click chooses it.
--
-- **Its keys are the events'** (`capture_key` in `wm.lua` hands them here
-- first): a Tab with Super held opens it, and while it shows every key is
-- its own - and every character, which `keys.lua` swallows - so nothing
-- reaches the window under it.
--
-- **The order last used is the stack's**: `windows` is bottom to top and a
-- window raised goes to the top, so the first time an application's window
-- is met from the top is how recently it was used. Nothing kept beside the
-- stack to fall out of step with it.
--
-- Drawn as a picture of its own, in a region (`gfx.surface{ shared = true
-- }`), which the scene blends over every window and under the pointer
-- (`scene:above`'s panel) - the window server composes it like the level
-- bar.
--

return function(ctx)
  local OUT, windows, raise, post, add_damage =
    ctx.OUT, ctx.windows, ctx.raise, ctx.post, ctx.add_damage
  local picture_from, px, W, H =
    ctx.picture_from, ctx.px, ctx.W, ctx.H

  local S = { showing = false }

  local TAB, LEFT, RIGHT, UP, DOWN, ESC, Q = 15, 105, 106, 103, 108, 1, 16
  local PANEL = 0xf0262a31          -- dark, nearly opaque (`switcher.html`)
  local LIT = 0x2effffff
  local NAME = 0xfff1f3f7

  local apps, lit, row, lit_win = {}, 1, false, 1
  local picture, at_x, at_y, pw, ph = nil, 0, 0, 0, 0
  local icons, icon_of = {}, {}

  local function chrome(win)
    return win.backdrop or win.strip or win.kind == "menu" or win.popup or win.tip
           or win.banner
  end

  -- The running applications, last used first: each its windows, front to
  -- back, its icon and its name.
  local function gather()
    local out, by_pid = {}, {}

    for i = #windows, 1, -1 do
      local win = windows[i]

      if not chrome(win) and win.pid then
        local a = by_pid[win.pid]

        if not a then
          a = { pid = win.pid, program = win.program, wins = {} }
          by_pid[win.pid] = a
          out[#out + 1] = a
        end

        a.wins[#a.wins + 1] = win
      end
    end

    for _, a in ipairs(out) do
      local title = tostring(a.wins[1].title or "")

      -- "Tracker - Home" is Tracker: the name before a dash, when there is one.
      a.name = title:match("^(.-)%s+%-%s+") or title:match("^(.-)%s+\u{2014}%s+") or title

      if a.program and icon_of[a.program] == nil then
        local attrs = fs.getattr(a.program)

        icon_of[a.program] = (attrs and attrs.icon) or "App_Generic"
      end

      a.icon = (a.program and icon_of[a.program]) or "App_Generic"
    end

    return out
  end

  -- An icon's 64-pixel picture, decoded once.
  local function icon(name)
    if icons[name] == nil then
      icons[name] = picture_from("64x64/" .. name .. ".png")
                    or picture_from("64x64/App_Generic.png") or false
    end

    return icons[name] or nil
  end

  -- The measures, at the screen's scale; smaller cells when many apps.
  local function measures()
    local cell, ic, gap, pad = px(104), px(84), px(10), px(18)

    if #apps * (cell + gap) + 2 * pad > W - px(80) then
      cell, ic, gap = px(76), px(60), px(6)
    end

    return cell, ic, gap, pad
  end

  local function measures_pad()
    local _, _, _, pad = measures()

    return pad
  end

  local function damage()
    if picture then add_damage(at_x, at_y, pw, ph) end
  end

  -- The panel drawn again, into a picture its size.
  local function draw()
    damage()

    local cell, ic, gap, pad = measures()
    local name_h = gfx.font.h + px(10)
    local thumbs = row and apps[lit] and apps[lit].wins or {}
    local tw, th = px(150), px(96)
    local row_h = row and (th + gfx.font.h + px(16)) or 0
    local w = math.max(2 * pad + #apps * cell + (#apps - 1) * gap,
                       row and (2 * pad + #thumbs * tw + (#thumbs - 1) * px(12)) or 0)
    local h = pad + cell + name_h + row_h + px(6)

    w = math.min(w, W - px(40))

    if not picture or pw ~= w or ph ~= h then
      if picture then picture:free() end

      picture = gfx.surface{ w = w, h = h, shared = true }
      pw, ph = w, h
    end

    picture:fill(0, 0, w, h, 0)
    picture:fill_round(0, 0, w, h, PANEL, px(22))

    local x0 = (w - (#apps * cell + (#apps - 1) * gap)) // 2

    for i, a in ipairs(apps) do
      local x = x0 + (i - 1) * (cell + gap)

      if i == lit then
        picture:fill_round(x, pad, cell, cell, LIT, px(18))

        local n = a.name
        local nw = gfx.measure(n)

        picture:text(x + (cell - nw) // 2, pad + cell + px(6), n, NAME, PANEL)
      end

      local pic = icon(a.icon)

      -- Stretched smooth and blended by its own alpha (255): without the
      -- alpha, `stretch` copies, and an icon's clear corners came out black.
      if pic then
        picture:stretch(pic, 0, 0, 64, 64, x + (cell - ic) // 2, pad + (cell - ic) // 2,
                        ic, ic, 255, true)
      end

      -- Dots for how many windows, up to three.
      if #a.wins > 1 then
        local n = math.min(3, #a.wins)
        local dx = x + (cell - (n * px(4) + (n - 1) * px(3))) // 2

        for k = 0, n - 1 do
          picture:fill_round(dx + k * px(7), pad + cell - px(8), px(4), px(4), 0xccd5dfe0, px(2))
        end
      end
    end

    if row then
      local y = pad + cell + name_h + px(6)
      local tx0 = (w - (#thumbs * tw + (#thumbs - 1) * px(12))) // 2

      for i, win in ipairs(thumbs) do
        local x = tx0 + (i - 1) * (tw + px(12))
        local src = win.shared and win.shared[win.shared.live] or win.surface

        if i == lit_win then
          picture:fill_round(x - px(3), y - px(3), tw + px(6), th + px(6), 0xff2f6bdc, px(10))
        end

        if src then
          local sw, sh = src:size()

          picture:stretch(src, 0, 0, sw, sh, x, y, tw, th, nil, true)
        end

        local t = tostring(win.title or "")

        while gfx.measure(t) > tw and #t > 1 do t = t:sub(1, -2) end

        picture:text(x, y + th + px(6), t, NAME, PANEL)
      end
    end

    at_x = (W - w) // 2
    at_y = (H * 46 // 100) - h // 2
    damage()
  end

  local function said()
    local names = {}

    for i, a in ipairs(apps) do names[#names + 1] = (i == lit) and ("[" .. a.name .. "]") or a.name end

    print("wm: switcher " .. table.concat(names, ", ")
          .. (row and (" - window " .. lit_win .. " of " .. #apps[lit].wins) or ""))
  end

  function S.open(back)
    apps = gather()

    if #apps == 0 then return end

    -- One Tab already lights the second: a tap and let go is "back to what
    -- I was just using".
    lit = (#apps > 1) and (back and #apps or 2) or 1
    row, lit_win = false, 1
    S.showing = true
    draw()

    -- Where it is and where each icon's middle is, for whoever drives it.
    local cell, _, gap = measures()
    local x0 = at_x + (pw - (#apps * cell + (#apps - 1) * gap)) // 2
    local mids = {}

    for i = 1, #apps do mids[i] = x0 + (i - 1) * (cell + gap) + cell // 2 end

    print(("wm: switcher at %d,%d %dx%d, icons at %s,%d"):format(
          at_x, at_y, pw, ph, table.concat(mids, "/"), at_y + measures_pad() + cell // 2))
    said()
  end

  function S.close()
    if not S.showing then return end

    damage()
    S.showing = false
    apps, row = {}, false
    print("wm: switcher put away")
  end

  local function step(d)
    if row then
      local n = #apps[lit].wins

      lit_win = (lit_win - 1 + d) % n + 1
    else
      lit = (lit - 1 + d) % #apps + 1
    end

    draw()
    said()
  end

  -- What was chosen, forward: the window chosen, or the application's
  -- windows back to front so the one used last is on top and has the keys.
  function S.commit()
    local a = apps[lit]

    if a then
      if row and a.wins[lit_win] then
        raise(a.wins[lit_win])
      else
        for i = #a.wins, 1, -1 do raise(a.wins[i]) end
      end

      print("wm: switcher chose " .. a.name)
    end

    S.close()
  end

  -- Q: the lit application asked to close each of its windows, as its close
  -- box asks, and taken off the panel.
  local function quit_lit()
    local a = apps[lit]

    if not a then return end

    for _, win in ipairs(a.wins) do
      win.closing = sys.ticks() + OUT.close_grace
      post(win, { type = "close" })
    end

    print("wm: switcher quit " .. a.name)
    table.remove(apps, lit)

    if #apps == 0 then S.close() return end

    lit = math.min(lit, #apps)
    row = false
    draw()
    said()
  end

  --
  -- A key, as an event: true when it was the switcher's. Closed, only a Tab
  -- with Super held is; showing, every key is, so none reaches a window.
  --
  function S.key_event(code, down)
    local held = OUT.chord.held
    local super = held[125] or held[126]
    local shift = held[42] or held[54]

    if not S.showing then
      if code == TAB and super then
        if down then S.open(shift) end
        return true
      end

      return false
    end

    if (code == 125 or code == 126) and not down then
      S.commit()
      return true
    end

    if not down then return true end

    if code == TAB then step(shift and -1 or 1)
    elseif code == RIGHT then step(1)
    elseif code == LEFT then step(-1)
    elseif code == DOWN and not row and #apps[lit].wins > 0 then
      row, lit_win = true, 1
      draw()
      said()
    elseif code == UP and row then
      row = false
      draw()
      said()
    elseif code == ESC then S.close()
    elseif code == Q then quit_lit()
    end

    return true
  end

  -- Which icon a point is over, or nil.
  local function cell_at(x, y)
    if not S.showing or x < at_x or y < at_y or x >= at_x + pw or y >= at_y + ph then
      return nil
    end

    local cell, _, gap, pad = measures()
    local x0 = at_x + (pw - (#apps * cell + (#apps - 1) * gap)) // 2

    if y < at_y + pad or y >= at_y + pad + cell then return nil end

    local i = (x - x0) // (cell + gap) + 1

    if i >= 1 and i <= #apps and (x - x0) % (cell + gap) < cell then return i end

    return nil
  end

  -- The pointer lights an icon.
  function S.hover(x, y)
    local i = cell_at(x, y)

    if i and i ~= lit and not row then
      lit = i
      draw()
    end
  end

  -- A press chooses the icon under it, or puts the switcher away; true
  -- while it shows, so the press reaches nothing under it.
  function S.press(x, y)
    if not S.showing then return false end

    local i = cell_at(x, y)

    print(("wm: switcher pressed at %d,%d, %s"):format(x, y, i and apps[i].name or "no icon"))

    if i then
      lit, row = i, false
      S.commit()
    elseif x < at_x or y < at_y or x >= at_x + pw or y >= at_y + ph then
      S.close()
    end

    return true
  end

  -- The picture for the scene, and where; nil when put away.
  function S.picture()
    if not S.showing then return nil, 0, 0 end

    return picture, at_x, at_y
  end

  return S
end
