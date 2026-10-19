-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- raise.lua TITLE: the first window whose title starts with TITLE, raised - for
-- bringing a window back over Telnet while measuring on a machine (18.709).
local wmproto = use("/Kosmos/Libraries/wmproto.lua")
local want = (arg and arg[1]) or "Cafesa3D"
local list = wmproto.windows()
for _, w in ipairs(list and list.windows or {}) do
  if tostring(w.title):sub(1, #want) == want then
    local reply = fs.send("/Running/wm", { type = "raise", window = w.handle or w.window or w.id })
    print("raised", w.title, reply and reply.ok)
    return
  end
end
print("no window", want)
