-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The window manager's level bar, a part of `wm.lua` in a file of its own
-- (`roadmap.md` 6zn). The section as it stood there, in a function handed
-- what it reads of the rest: the screen's width, which never changes in
-- that process; `add_damage`; and `reserved_top`, asked each time because
-- the Deskbar changes it - a copy taken here would go stale.
--
-- The level bar: the volume, and the brightness when there is one, shown
-- over everything.
--
-- `docs/levels.html`, drawn after macOS's Display and Sound panels and
-- approved on 18 September - "all is good", "i like the bar with smooth
-- instead of notches": a dark rounded panel titled by what it controls, a
-- small and a large icon either side of a smooth track with a knob, top
-- right under the bar. It appears when a level changes and fades two
-- seconds after the last change.
--
-- **Drawn here, by the window manager, from what it already knows.** The
-- keys are the system's, taken in `volume_key`, so the panel moves at the
-- moment of the key rather than after a round trip - the rule every control
-- here follows. It is drawn once into a surface of its own when the level
-- changes; each frame only blends that surface over the windows, with a
-- global alpha for the fade.
--
-- **Built so nothing is drawn twice.** `fill` replaces pixels and `disc`
-- blends only its anti-aliased edge, so a rounded shape is four corner discs
-- first and three rectangles over them: the rectangles replace whatever the
-- discs left inside, and the corners keep their smooth edge. Everything
-- inside the panel is at the panel's own opacity, pre-mixed, because a
-- colour with less alpha written in by replacement would be a hole in it.
--
--
-- **One table, because `wm.lua`'s main chunk is at Lua's limit of two
-- hundred locals** - this first went in as twenty of them, and the file
-- stopped loading. Everything about the level bar lives here.
--

return function(ctx)
  local osd = {
    W = 300, H = 74, R = 18,
    HOLD = 2.0,                           -- seconds after the last change
    FADE = 0.18,                          -- seconds of fading out
    HZ = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000,

    EDGE  = 0xe63a3a3a,                   -- the rim
    BODY  = 0xe61e1e1e,                   -- the panel, at nine tenths
    RAIL  = 0xe6484848,                   -- the track's empty part, pre-mixed
    INK   = 0xffffffff,
    KNOB  = 0xfff2f2f2,
    QUIET = 0xe68c8c8c,                   -- the fill while muted

    surface = nil, shown = false, until_at = 0, x = 0, y = 0,
  }

  function osd.rounded(s, x, y, w, h, r, colour)
    s:disc(x + r, y + r, r, colour)
    s:disc(x + w - r - 1, y + r, r, colour)
    s:disc(x + r, y + h - r - 1, r, colour)
    s:disc(x + w - r - 1, y + h - r - 1, r, colour)
    s:fill(x + r, y, w - 2 * r, h, colour)
    s:fill(x, y + r, r, h - 2 * r, colour)
    s:fill(x + w - r, y + r, r, h - 2 * r, colour)
  end

  -- A line two pixels thick, as two triangles, for the mute's cross.
  function osd.line(s, x1, y1, x2, y2, colour)
    s:triangle(x1 - 1, y1, x1 + 1, y1, x2 + 1, y2, colour)
    s:triangle(x1 - 1, y1, x2 + 1, y2, x2 - 1, y2, colour)
  end

  -- A speaker: a box and a cone, and `waves` arcs made as crescents - a disc
  -- of ink with one of the panel's colour over it, two pixels to the left.
  function osd.speaker(s, x, cy, waves, muted, colour)
    for i = waves, 1, -1 do
      s:disc(x + 9, cy, 2 + 4 * i, colour)
      s:disc(x + 7, cy, 2 + 4 * i, osd.BODY)
    end

    s:fill(x, cy - 3, 4, 7, colour)
    s:triangle(x + 3, cy - 3, x + 9, cy - 8, x + 9, cy + 8, colour)
    s:triangle(x + 3, cy - 3, x + 9, cy + 8, x + 3, cy + 3, colour)

    if muted then
      osd.line(s, x + 12, cy - 5, x + 20, cy + 5, colour)
      osd.line(s, x + 12, cy + 5, x + 20, cy - 5, colour)
    end
  end

  -- A sun: a disc and eight dots around it.
  function osd.sun(s, cx, cy, core, reach, dot, colour)
    s:disc(cx, cy, core, colour)

    for i = 0, 7 do
      local a = i * math.pi / 4

      s:disc(math.floor(cx + math.cos(a) * reach + 0.5),
             math.floor(cy + math.sin(a) * reach + 0.5), dot, colour)
    end
  end

  --
  -- Shown, or shown again: `which` is "sound" or "display", `level` 0 to 1.
  -- Muted keeps the level and greys the fill, as `levels.html` draws it.
  --
  function osd.show(which, level, muted)
    if not osd.surface then
      osd.surface = gfx.surface{ w = osd.W, h = osd.H }

      if not osd.surface then return end
    end

    local s, w, h = osd.surface, osd.W, osd.H

    s:fill(0, 0, w, h, 0x00000000)
    osd.rounded(s, 0, 0, w, h, osd.R, osd.EDGE)
    osd.rounded(s, 1, 1, w - 2, h - 2, osd.R - 1, osd.BODY)

    s:text(18, 10, which == "display" and "Display" or "Sound", osd.INK)

    local cy = 50
    local tx, tw = 48, w - 48 - 50

    if which == "display" then
      osd.sun(s, 26, cy, 3, 7, 1, osd.INK)
      osd.sun(s, w - 28, cy, 4, 10, 2, osd.INK)
    else
      osd.speaker(s, 18, cy, muted and 0 or 1, muted, osd.INK)
      osd.speaker(s, w - 40, cy, 3, false, osd.INK)
    end

    level = math.max(0, math.min(1, level or 0))

    local fw = math.max(6, math.floor(tw * level + 0.5))

    osd.rounded(s, tx, cy - 3, tw, 6, 3, osd.RAIL)
    osd.rounded(s, tx, cy - 3, fw, 6, 3, muted and osd.QUIET or osd.INK)

    local kx = math.max(tx, math.min(tx + tw - 26, tx + fw - 13))

    osd.rounded(s, kx, cy - 8, 26, 16, 8, osd.KNOB)

    -- Top right, under the bar: where macOS puts it, and where the drawing
    -- does.
    if osd.shown then ctx.add_damage(osd.x, osd.y, w, h) end

    osd.x = ctx.width - w - 14
    osd.y = ctx.reserved_top() + 10
    osd.until_at = sys.ticks() + math.floor(osd.HOLD * osd.HZ)
    osd.shown = true

    ctx.add_damage(osd.x, osd.y, w, h)
  end

  -- How opaque it is now: whole until its time is up, then fading to nothing.
  function osd.alpha(now)
    if not osd.shown then return 0 end

    local left = osd.until_at - now

    if left > 0 then return 255 end

    local gone = -left / (osd.FADE * osd.HZ)

    if gone >= 1 then return 0 end

    return math.floor(255 * (1 - gone))
  end

  -- Each pass: the fade drawn a frame at a time, and the panel gone at its end.
  function osd.tick()
    if not osd.shown then return end

    local now = sys.ticks()

    if now >= osd.until_at then
      ctx.add_damage(osd.x, osd.y, osd.W, osd.H)

      if osd.alpha(now) == 0 then osd.shown = false end
    end
  end

  return osd
end
