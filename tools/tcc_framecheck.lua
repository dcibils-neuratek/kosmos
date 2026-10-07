-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: image build/framecheck.elf
--
-- The Window Kit's frame path, held (`tools/tcc_framecheck.c`,
-- `user/include/wmproto.h`): what 100 frames allocate in Lua, and the window
-- manager answering three requests it must not act on - an operation the
-- shape has not got, a request too short, a window that is not there - each
-- with its reason.
local TAG = 0x31454d4152464d57                   -- "WMFRAME1"
local wm = fs.capability("/Running/wm")
local function ask(bytes)
  local reply = sys.call_raw(wm, bytes, -1, TAG)
  return reply and #reply >= 12 and string.unpack("<i4", reply) or -1
end
local bytes = use("framecheck.elf").frames()
local bad_op = ask(string.pack("<I4I4i4i4I4I4I4", 99, 1, 0, 0, 0, 0, 0))
local short = ask(string.pack("<I4I4", 2, 1))
local missing = ask(string.pack("<I4I4i4i4I4I4I4", 2, 0x7fffffff, 0, 0, 0, 0, 0))
print(("FRAMECHECK %s bytes over 100 frames; op 99 %d; short %d; no window %d")
      :format(tostring(bytes), bad_op, short, missing))
