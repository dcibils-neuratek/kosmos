-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The window manager's side of the manager door: the window server, which
-- composes the desktop this process describes (`docs/astra-display.md`
-- D3b-2, `user/servers/windowserver.c`, `user/include/windowproto.h`).
--
-- **Everything it composes is handed to it once, as a region**: the back
-- buffer when this starts, the region the scene is written in, and each
-- picture the scene names - a window's two buffers, and this process's own
-- wallpaper, stamp, menu bars, level bar, arrow and a drag's label, each a
-- surface made `shared` - under a slot this file chooses. A pass then
-- names pictures by slot (`pic`), writes the scene into the region they
-- share (`scene:wire`) and asks for it composed, in one call.
--
-- **A picture goes when its surface does**: freed - a window's buffers are
-- when its region is let go - or collected. Each pass looks at the slots
-- held and lets the server forget those, so it never holds a region this
-- process has finished with for longer than a pass.
--
-- **There is no desktop without one** (step 4): the window manager no
-- longer composes or touches the screen itself, so with no window server -
-- a machine with no screen, or a window manager started where the door is
-- not lent - `WS.w` is nil and it ends, saying why.
--

local REQUEST = "<I4I4I4I4I4I4I8"
local REPLY = "<I4I4I4I4I4I4"

local OP = { hello = 1, back = 2, scene = 3, picture = 4, forget = 5, compose = 6 }
local SHOW = 1
local ERRORS = {
  [1] = "a request it does not know",
  [2] = "it has no screen",
  [3] = "no region came, or it would not map",
  [4] = "the region does not hold what was said",
  [5] = "it has no memory for that",
  [6] = "composing before a back buffer and a scene",
  [7] = "the scene says more than its region holds",
}

-- The pages a region of `bytes` takes.
local function pages(bytes) return (bytes + 4095) // 4096 end

return function()
  local back
  local WS = { on = false }

  -- One request: true and its reply's three words, or nil and why.
  local function ask(bytes, cap)
    local ok, reply, why = pcall(fs.raw, "/Windowserver", bytes, cap, "windowserver")

    if not ok then return nil, tostring(reply) end
    if type(reply) ~= "string" or #reply < 24 then return nil, tostring(why or reply) end

    local err, a, b, c, d, e = string.unpack(REPLY, reply)

    if err ~= 0 then return nil, ERRORS[err] or ("error " .. err) end

    return true, a, b, c, d, e
  end

  local function request(op, slot, w, h, pitch, flags, offset)
    return string.pack(REQUEST, op, slot or 0, w or 0, h or 0, pitch or 0, flags or 0,
                       offset or 0)
  end

  --
  -- The pictures: each surface's slot, and each slot's surface - weakly,
  -- so a picture this process dropped can still be collected, and `held`
  -- remembers which slots the server has so the gone ones can be told.
  --
  local slot_of = setmetatable({}, { __mode = "k" })
  local surface_in = setmetatable({}, { __mode = "v" })
  local held, spare, next_slot = {}, {}, 0
  local said_private = false

  local function forget(slot)
    ask(request(OP.forget, slot))
    held[slot], surface_in[slot] = nil, nil
    spare[#spare + 1] = slot
  end

  --
  -- `s`'s slot, handing it over the first time it is named. Nil for a
  -- surface with no region of its own, which cannot be composed there: it
  -- is said once, since it is a picture this process forgot to make
  -- `shared`.
  --
  function WS.pic(s)
    if s == nil then return nil end

    local slot = slot_of[s]

    if slot then return slot end

    local cap, offset, pitch = s:region()

    if not cap then
      if not said_private then
        said_private = true
        print(("wm: a %dx%d picture with no region of its own is not composed by the "
               .. "window server"):format(s:size()))
      end

      return nil
    end

    slot = table.remove(spare)

    if not slot then
      slot = next_slot
      next_slot = next_slot + 1
    end

    local w, h = s:size()
    local ok, why = ask(request(OP.picture, slot, w, h, pitch, 0, offset), cap)

    if not ok then
      spare[#spare + 1] = slot
      print(("wm: the window server was not given a %dx%d picture: %s"):format(w, h, why))
      return nil
    end

    slot_of[s], surface_in[slot], held[slot] = slot, s, true
    return slot
  end

  -- The slots whose surface is freed or gone, forgotten.
  local function sweep()
    for slot in pairs(held) do
      local s = surface_in[slot]

      if s == nil or not s:alive() then forget(slot) end
    end
  end

  --
  -- The region the scene is written in: made at `bytes`, handed over, and
  -- the old one let go - grown when a scene does not fit.
  --
  local scene_at, scene_bytes, scene_cap = nil, 0, nil

  local function scene_region(bytes)
    local n = pages(bytes)
    local cap, why = sys.memory(n)

    if not cap then return nil, why end

    local at, err = sys.memory_map(cap)

    if not at then
      sys.release(cap)
      return nil, err
    end

    local ok, refused = ask(request(OP.scene), cap)

    if not ok then
      sys.release(cap)
      return nil, refused
    end

    if scene_cap then sys.release(scene_cap) end

    scene_at, scene_bytes, scene_cap = at, n * 4096, cap
    return true
  end

  --
  -- The damage, composed there, and with `show` put on the screen: the
  -- pixels drawn and the pictures it holds, or nil and why.
  --
  local COMPOSE = { [true] = request(OP.compose, 0, 0, 0, 0, SHOW),
                    [false] = request(OP.compose) }

  function WS.compose(scene, damage, show)
    sweep()

    local ok, need = scene:wire(scene_at, scene_bytes, damage)

    if not ok then
      local grown, why = scene_region(need * 2)

      if not grown then return nil, why end

      ok = scene:wire(scene_at, scene_bytes, damage)
      if not ok then return nil, "the scene does not fit its region" end
    end

    local done, why, _, drawn, compose_us, copy_us = ask(COMPOSE[show == true])

    if not done then return nil, why end

    -- And how many pictures it holds now, the first word of the reply -
    -- said when it changes, which a window opened or closed does: what the
    -- window server holds should come back down when the window goes.
    if why ~= WS.held then
      WS.held = why
      print(("wm: the window server holds %d pictures"):format(why))
    end

    return drawn, why, compose_us, copy_us
  end

  --
  -- Greeted first, for the screen's size, which the back buffer is made
  -- at; then handed it (`WS.connect`) and a scene's region.
  --
  --
  -- **With an endpoint of this process's own**, which goes when it does:
  -- the window server watches it, and gives the screen back to the
  -- console when it names nothing - a window manager that crashed cannot
  -- give anything back itself.
  --
  WS.alive = sys.endpoint()

  local hello, w, h = ask(request(OP.hello), WS.alive)

  if not hello then
    WS.why = tostring(w)
    return WS
  end

  WS.w, WS.h = w, h
  print(("wm: the window server answers, the screen %dx%d"):format(w, h))

  --
  -- The back buffer handed over, and a region for the scene: true, or
  -- false and why.
  --
  function WS.connect(surface)
    back = surface

    local cap, _, pitch = back:region()
    local bw, bh = back:size()
    local ok, why = nil, "the back buffer has no region of its own"

    if cap then ok, why = ask(request(OP.back, 0, bw, bh, pitch), cap) end
    if ok then ok, why = scene_region(16384) end

    if not ok then return false, why end

    WS.on = true
    print(("wm: the window server composes, %dx%d"):format(bw, bh))
    return true
    end

  return WS
end
