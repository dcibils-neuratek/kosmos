-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- The shape of a `poll` to the window manager, in one place.
--
-- `design.md` §17's argument, one protocol at a time: what crosses into a
-- server is a *declared shape* rather than whatever somebody put in a
-- table. `user/include/audioproto.h` is that for `/dev/audio` and this is
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
-- `/dev/cpu` three lines above, where the unit is visible. This was the
-- only one where the number was *mailed to another process*, and a message
-- carries no units.
--
-- So `wait_ticks` says what it is, and `poll` below is the only place that
-- writes it.
--
local wmproto = {}

-- The window manager's path in the namespace. Named once so a caller does
-- not repeat a string the server could rename.
wmproto.WM = "/app/wm"

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
function wmproto.poll(handle, wait_ticks)
  return fs.send(wmproto.WM, {
    type = "poll",
    window = handle,
    wait_ticks = wait_ticks or 0,
  })
end

--
-- The clipboard.
--
-- **One buffer, held by the window manager**, for the reason it holds the
-- screen: a clipboard is shared between programs that cannot reach each
-- other, so it belongs to the one process both of them already talk to.
-- No global name and no shared page - an application that was not given
-- `/app/wm` has no clipboard, which is the right answer rather than a
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

return wmproto
