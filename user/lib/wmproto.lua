-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- The shape of a `poll` to the window manager, in one place.
--
-- `design.md` §17's argument, one protocol at a time: what crosses into a
-- server is a *declared shape* rather than whatever somebody put in a
-- table. `user/include/audioproto.h` is that for `/Devices/audio` and this is
-- the same idea where both sides are Lua - a module both of them name,
-- rather than a header both of them compile against. It cannot make a
-- wrong field a compile error the way the C headers can. What it can do is
-- put the field's *unit in its name* and leave exactly one place where the
-- message is built.
--
-- **Which is the whole reason it exists.** The field was `wait`, and this
-- system has two clocks: `sys.ticks()` is the counter, tens of megahertz,
-- and every timeout - `sys.sleep`, `sys.receive`, `fs.wait_input` - is in
-- scheduler ticks at 250 Hz. Eight call sites built the message by hand
-- and all eight wrote `wait = 1` meaning one scheduler tick. The window
-- manager added that 1 to `sys.ticks()`. A factor of a quarter of a
-- million, so every animating window asked to be woken in sixteen
-- nanoseconds, which is to say immediately, and was answered whenever the
-- window manager's own loop next came round.
--
-- What that looked like: the cube ran *faster while the mouse was moving*,
-- because pointer events cut the manager's sleep short and it passed more
-- often. And `music`, which hands over an audio period when its poll
-- returns, stuttered - a feed later than 23 ms is a hole you can hear.
--
-- Every other place in the tree that does arithmetic on `sys.ticks()` is
-- correct, and correct for one reason: it reads `counter_hz` from
-- `/Devices/cpu` three lines above, where the unit is visible. This was the
-- only one where the number was *mailed to another process*, and a message
-- carries no units.
--
-- So `wait_ticks` says what it is, and `poll` below is the only place that
-- writes it.
--
local wmproto = {}

-- The window manager's path in the namespace. Named once so a caller does
-- not repeat a string the server could rename.
wmproto.WM = "/Running/wm"

--
-- **Every window, as one list**, from the pages the window manager answers
-- `windows` in: a list of seventeen did not fit in one message, and its
-- reply was dropped (`wm.lua`, `handlers.windows`). `watch` is passed on
-- the first page, as it always was. Answers what one reply used to - `ok`
-- and `windows` - or nil and why.
--
-- A window that opens or closes between two pages moves the rest by one;
-- the caller may then see one twice or miss one, until it asks again - and
-- the Deskbar, the one that watches, is told to.
--
function wmproto.windows(watch)
  local all, from, starting = {}, 1, nil

  while from do
    local reply, why = fs.send(wmproto.WM, { type = "windows", from = from,
                                             watch = (from == 1) and watch or nil })

    if not reply then return nil, why end
    if not reply.ok then return nil, reply.error end

    for _, w in ipairs(reply.windows or {}) do all[#all + 1] = w end

    -- What is starting (`docs/launching.html`), the same on every page.
    starting = starting or reply.starting
    from = tonumber(reply.more)
  end

  return { ok = true, windows = all, starting = starting or {} }
end

--
-- The pointer's movement with no button held, while `on` - for an
-- operation that follows the pointer until a click, as Blender's G does.
--
function wmproto.track(handle, on)
  return fs.send(wmproto.WM, { type = "track", window = handle, on = on and true or false })
end

--
-- Ask for events, blocking up to `wait_ticks` *scheduler* ticks.
--
-- Zero means "answer immediately, empty if there is nothing", which is what
-- `doom` wants: it drives its own frame clock and a block would be a stall.
-- Anything above zero is a real block, and the manager holds the reply -
-- the caller is a descheduled thread until then, costing nothing.
--
-- Returns whatever the manager replied, or nil when it has gone away, which
-- is the ordinary end of an application here.
--
--
-- **Windows whose band the kit draws** (one window chrome, step 2): a
-- direct window's header is drawn into the top of its surface by `ui.lua`,
-- which registers it here, and its events go through `band_events` - the
-- pointer moved up past the band, a press on it taken - before the
-- application, which polls with this as it always has, sees them.
--
wmproto.banded = {}

function wmproto.poll(handle, wait_ticks)
  local reply = fs.send(wmproto.WM, {
    type = "poll",
    window = handle,
    wait_ticks = wait_ticks or 0,
  })

  local win = wmproto.banded[handle]

  if win and reply and reply.events then win:band_events(reply.events) end

  return reply
end

--
-- The clipboard.
--
-- **One buffer, held by the window manager**, for the reason it holds the
-- screen: a clipboard is shared between programs that cannot reach each
-- other, so it belongs to the one process both of them already talk to.
-- No global name and no shared page - an application that was not given
-- `/Running/wm` has no clipboard, which is the right answer rather than a
-- missing feature.
--
-- **The cap is enforced here, on the way out, and that is not a detail.**
-- The text travels inside a message and `MSG_BYTES` is 2048, so a copy
-- larger than that does not arrive truncated - `fs.send` *raises*, and an
-- application that copied a long report dies where it stood. Capping in
-- the server would be too late by one process.
--
-- 1900 leaves room for the rest of the message. A table with two string
-- fields costs a tag and a length for each of the four items plus one
-- byte to open it and one to close, which is about thirty-five - so this
-- is comfortable rather than exact, and being exact would mean tying a
-- protocol constant to the serialiser's encoding.
--
-- `copy` returns how many bytes were actually taken, and how many were
-- not. Both, because a clipboard that silently holds half of what you
-- copied is worse than one that says so, and only the caller knows how to
-- say it: `ui.editor` shrinks the highlight to what fits, which is the
-- screen telling the truth without a word of prose.
--
wmproto.CLIP_MAX = 1900

function wmproto.copy(text)
  text = tostring(text or "")

  local taken = text:sub(1, wmproto.CLIP_MAX)
  local r = fs.send(wmproto.WM, { type = "clip_put", text = taken })

  if not r or not r.ok then return nil end

  return r.bytes, #text - #taken
end

--
-- What was last copied, or "" when nothing has been. Never nil on success,
-- so a caller can paste it without testing.
--
function wmproto.paste()
  local r = fs.send(wmproto.WM, { type = "clip_get" })
  if not r or not r.ok then return nil end
  return r.text or ""
end

--------------------------------------------------------------------------
-- **The screen, lent** (`wm.lua`, `remote`): `/Running/wm/remote`, which
-- the window manager mounts only in a program it launches whose header says
-- `kosmos: needs desktop`. The window manager owns every pixel, so the
-- screen is asked of it, never read from the framebuffer: a region of this
-- process the screen's size, handed over, and filled with each frame as it
-- is composed. `screenshot` and `vncd` each made that handshake by hand.
--------------------------------------------------------------------------

wmproto.REMOTE = "/Running/wm/remote"

--
-- A copy of the screen, not yet watched: `{ w, h, cap, at, surface }` -
-- `surface` the region's pixels to read - or nil, why, and whether the
-- desktop lent its screen at all, which is when a caller has something to
-- add about where it must be started from. The window manager says the
-- size first, and the region is made to it.
--
function wmproto.screen()
  local size = fs.send(wmproto.REMOTE, { type = "watch" })

  if type(size) ~= "table" or not size.w then
    return nil, "the desktop did not lend its screen", false
  end

  local copy, why = use("/Kosmos/Libraries/regions.lua").make(size.bytes)

  if not copy then return nil, "no memory for a copy of the screen: " .. tostring(why), true end

  return { w = size.w, h = size.h, cap = copy.cap, at = copy.at, watching = false,
           surface = gfx.wrap{ at = copy.at, w = size.w, h = size.h } }
end

--
-- **Watched**: the copy handed to the window manager, which fills it whole
-- at once and then with every frame. True, or nil and why. It lets the copy
-- go by itself when nobody has asked `watched` for five seconds, and then
-- this is asked again with the same copy.
--
function wmproto.watch(screen)
  local r = fs.send(wmproto.REMOTE, { type = "watch" }, screen.cap)

  if type(r) ~= "table" or not r.ok then
    return nil, "the desktop would not share its screen: "
                .. tostring(type(r) == "table" and r.error or r)
  end

  screen.watching = true
  return true
end

--
-- The rectangles that changed since last asked, each `{ x, y, w, h }`; nil
-- when the window manager has let the copy go, which `screen.watching`
-- then says too.
--
function wmproto.watched(screen)
  local r, why = fs.send(wmproto.REMOTE, { type = "watched" })

  --
  -- **Let go only when the window manager says so.** Any failure used to
  -- mean "watch again", and watching again is the whole screen copied: when
  -- `watched` itself failed (a rectangle it could not pack, the M700, 8
  -- October), every pass became a full copy. Now only "nothing is watched"
  -- is a copy let go; anything else is said once and the copy kept.
  --
  if type(r) ~= "table" or type(r.rects) ~= "string" then
    if why == "nothing is watched" or not screen.watching then
      screen.watching = false
      return nil
    end

    if screen.said ~= why then
      screen.said = why
      print("wmproto: the screen's changes could not be had: " .. tostring(why))
    end

    return {}
  end

  local list = {}

  for at = 1, #r.rects - 7, 8 do
    local x, y, w, h = string.unpack(">I2I2I2I2", r.rects, at)
    list[#list + 1] = { x, y, w, h }
  end

  return list
end

return wmproto
