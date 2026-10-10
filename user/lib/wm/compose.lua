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

  local function compose()
    if #damage == 0 then return end

    if P.measuring then
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

    describe()

    local drawn, why = ws.compose(scene, damage, true)

    if not drawn then
      print("wm: the window server did not compose: " .. tostring(why))
    elseif P.measuring then
      P.prof.drawn = P.prof.drawn + drawn
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
