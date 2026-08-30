-- init: two processes, one endpoint, and a Lua table crossing between them.
--
-- The same image runs as both. The kernel tells each one which it is, and
-- hands each a capability for the same endpoint; neither can name anything
-- else, because a capability table is all a process has.

local role = ...
local EP = 0            -- by convention, a process's first capability

local function line(s) sys.write(s .. "\n") end

if role == 1 then
  ---------------------------------------------------------------- server
  while true do
    local request, sender = sys.receive(EP)
    if not request then break end   -- the endpoint went away

    -- A table arrived. Not a byte array with an agreed layout: a table,
    -- with strings, floats, integers, booleans and nesting intact.
    local reply = {
      tag     = (request.tag or 0) + 1,
      echoed  = request.name,
      doubled = request.n * 2,
      float   = request.pi * 2,
      nested  = { deep = { value = request.nested.deep.value .. "!" } },
      list    = {},
    }
    for i, v in ipairs(request.list) do reply.list[i] = v * v end

    sys.reply(sender, reply)
  end
else
  ---------------------------------------------------------------- client
  line("init: Lua " .. _VERSION .. " at EL0, two processes")

  local request = {
    tag    = 7,
    name   = "kosmos",
    n      = 21,
    pi     = 3.5,
    flag   = true,
    nested = { deep = { value = "hello" } },
    list   = { 1, 2, 3, 4 },
  }

  local reply, err = sys.call(EP, request)
  if not reply then
    line("init: call failed: " .. tostring(err))
    return
  end

  line("init: tag     " .. reply.tag)
  line("init: echoed  " .. reply.echoed .. "  (a string, round trip)")
  line("init: doubled " .. reply.doubled .. "  (integer stayed integer: "
       .. math.type(reply.doubled) .. ")")
  line("init: float   " .. reply.float .. "  (float stayed float: "
       .. math.type(reply.float) .. ")")
  line("init: nested  " .. reply.nested.deep.value)
  line("init: list    " .. table.concat(reply.list, ", "))

  -- What must not cross. A function means nothing in a state that did not
  -- create it, so the serialiser refuses it rather than inventing something.
  local ok, e = pcall(function()
    return sys.call(EP, { fn = function() end })
  end)
  line("init: sending a function -> " .. tostring(ok) .. ", " .. tostring(e))

  -- And a cycle, which is bounded by depth rather than chased.
  local cyclic = {}
  cyclic.self = cyclic
  local ok2, e2 = pcall(function() return sys.call(EP, cyclic) end)
  line("init: sending a cycle    -> " .. tostring(ok2) .. ", " .. tostring(e2))

  line("init: done")
end
