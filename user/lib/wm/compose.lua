-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Compositing, as the window manager does it now: describe the scene and
-- ask the window server to compose the damage (`docs/astra-display.md`
-- D3b-2) - a part of `wm.lua` in a file of its own (`roadmap.md` 6zn). The
-- damage list is emptied in place at the end of a pass, since `add_damage`
-- holds the same one.
--
-- **The composing itself is not here any more** (step 4). It was: a pass
-- over each damaged rectangle working out, front to back, what each window
-- shows of it, then drawing the desktop and the windows back to front with
-- their shadows, corners, lights, menus, the outline, the level bar and the
-- arrow. That moved to C in the Graphics Kit's scene (D3a) - held to these
-- Lua passes pixel for pixel by `wm compose-check` over thousands of passes
-- - and then into the window server's process (D3b-2), where Lua cannot
-- follow it. `git log` has the Lua for whoever wants to read it.
--

return function(ctx)
  local scene, describe, ws, P, damage, mirror =
    ctx.scene, ctx.describe, ctx.ws, ctx.P, ctx.damage, ctx.mirror

  --
  -- **Overlapping damage merged** before it is composed (`testing.md`
  -- 18.663): two rectangles that overlap become their bounding box when
  -- that box is no larger than the two of them together - so nothing is
  -- composed twice, and nothing much is composed that was not damaged. A
  -- window dragged while a frame waits for its flip adds its old and new
  -- places at every mouse report, and unmerged that was ten million pixels
  -- composed a frame on a screen of two and a half. In place, since
  -- `add_damage` holds this same list.
  --
  local function merge()
    local merged = true

    while merged do
      merged = false

      for i = #damage, 2, -1 do
        local a = damage[i]

        for j = i - 1, 1, -1 do
          local b = damage[j]
          local x0, y0 = math.min(a.x, b.x), math.min(a.y, b.y)
          local x1 = math.max(a.x + a.w, b.x + b.w)
          local y1 = math.max(a.y + a.h, b.y + b.h)

          if a.x < b.x + b.w and b.x < a.x + a.w and a.y < b.y + b.h and b.y < a.y + a.h
             and (x1 - x0) * (y1 - y0) <= a.w * a.h + b.w * b.h then
            b.x, b.y, b.w, b.h = x0, y0, x1 - x0, y1 - y0
            table.remove(damage, i)
            merged = true
            break
          end
        end
      end
    end
  end

  local function compose()
    if #damage == 0 then return end

    merge()

    describe()

    local drawn, why, compose_us, copy_us, held_back = ws.compose(scene, damage, true)

    -- **Not now** (`m700-2d.md` step 4b): the screen has a flip on its way,
    -- so nothing was composed - the damage stays for the next pass, which
    -- `sleep_for` makes come within a tick, and nothing is counted.
    if held_back then return end

    if drawn and P.measuring then
      P.prof.frames = P.prof.frames + 1
      P.prof.rects  = P.prof.rects + #damage

      --
      -- **`px` is what the compositor was asked for; `drawn` is what it
      -- did** - the area of the damage, and what the window server drew of
      -- it, which occlusion makes less and shadows and corners more. The
      -- ratio between them is overdraw.
      --
      for _, r in ipairs(damage) do P.prof.px = P.prof.px + r.w * r.h end
    end

    if not drawn then
      print("wm: the window server did not compose: " .. tostring(why))
    elseif P.measuring then
      P.prof.drawn = P.prof.drawn + drawn
      -- The window server's own two halves, in microseconds: composing
      -- into the back buffer, and the copy from it to the screen
      -- (`m700-2d.md` step 4a) - what page flips could take away.
      P.prof.ws_compose_us = P.prof.ws_compose_us + (compose_us or 0)
      P.prof.ws_copy_us = P.prof.ws_copy_us + (copy_us or 0)
      if (copy_us or 0) > P.prof.ws_copy_max then P.prof.ws_copy_max = copy_us end
    end

    -- On the screen already; to whoever watches it from here, from the back
    -- buffer the window server composed into.
    for _, r in ipairs(damage) do mirror(r) end

    -- Emptied where it is rather than replaced: `add_damage` in `wm.lua`
    -- holds this same list, and a new one here would be a list only this
    -- file had (`roadmap.md` 6zn).
    for i = #damage, 1, -1 do damage[i] = nil end
  end

  return compose
end
