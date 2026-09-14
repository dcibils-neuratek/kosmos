-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The block protocol from Lua: a USB stick's blocks, through the USB driver.
--
-- **The layout below is `blockproto.h` written a second time**, as `audio.lua`
-- writes `audioproto.h` a second time and for its reason: the server is C and
-- this is Lua, so one holds the shape as a struct and the other as a format
-- string. The sizes are asserted when this loads, and the driver refuses a
-- request of any other length, so a disagreement is loud.
--
-- **The blocks do not come back in the reply.** `open` creates a region and
-- hands it to the driver once; a read fills it, and this copies out what was
-- read. Read only, for now: the driver refuses a write until step 5e.

local blocks = {}

local OP_INFO, OP_OPEN, OP_READ, OP_CLOSE = 1, 2, 3, 5

-- struct block_request: op, unit, lba, count, handle
local REQUEST  = "<I4I4I8I4I4"
local REQ_SIZE = 24

-- struct block_reply: error, block_size, blocks, count, handle, vendor, product
local REPLY      = "<I4I4I8I4I4c8c16"
local REPLY_SIZE = 48

assert(#string.pack(REQUEST, 0, 0, 0, 0, 0) == REQ_SIZE,
       "blocks: the request layout does not match blockproto.h")
assert(#string.pack(REPLY, 0, 0, 0, 0, 0, "", "") == REPLY_SIZE,
       "blocks: the reply layout does not match blockproto.h")

-- `BLOCK_TRANSFER_MOST`: the most one read moves, and the region's size.
blocks.TRANSFER_MOST = 31 * 4096

--
-- An error is a number on the wire and a sentence here, as in `audio.lua`.
--
local ERRORS = {
  [1] = "the USB driver did not understand that",
  [2] = "no stick at that unit",
  [3] = "that was not opened",
  [4] = "more blocks than one read can move",
  [5] = "that block is past the last",
  [6] = "the stick failed it",
  [7] = "writing is not allowed yet",
  [8] = "every open slot is taken",
}

local function trimmed(s)
  return (s:gsub("[%z ]+$", ""))
end

local function request(op, fields, pass)
  local bytes = string.pack(REQUEST, op, fields.unit or 0, fields.lba or 0,
                            fields.count or 0, fields.handle or 0)
  local reply, why = fs.raw("/dev/blocks", bytes, pass)

  if not reply then return nil, tostring(why) end

  if #reply < REPLY_SIZE then
    return nil, "the USB driver sent a reply of the wrong size"
  end

  local err, size, count, moved, handle, vendor, product =
      string.unpack(REPLY, reply)

  if err ~= 0 then
    return nil, ERRORS[err] or ("block error " .. tostring(err))
  end

  return { block_size = size, blocks = count, count = moved, handle = handle,
           vendor = trimmed(vendor), product = trimmed(product) }
end

-- What a unit is: how many blocks, how long one is, and what it says it is.
function blocks.info(unit)
  return request(OP_INFO, { unit = unit })
end

local reader = {}
reader.__index = reader

-- A region for reads, created here and handed to the driver once.
function blocks.open()
  local cap, why = sys.memory(blocks.TRANSFER_MOST // 4096)

  if not cap then return nil, tostring(why) end

  local r, err = request(OP_OPEN, {}, cap)

  if not r then
    sys.release(cap)
    return nil, err
  end

  return setmetatable({ cap = cap, handle = r.handle }, reader)
end

-- `count` blocks from `lba` of `unit`, as a string of bytes.
function reader:read(unit, lba, count)
  local r, why = request(OP_READ, { unit = unit, lba = lba, count = count,
                                    handle = self.handle })

  if not r then return nil, why end

  return sys.region_read(self.cap, 0, r.count * r.block_size)
end

-- The region given back, to the driver and then to the machine.
function reader:close()
  local r, why = request(OP_CLOSE, { handle = self.handle })

  sys.release(self.cap)

  if not r then return nil, why end

  return true
end

return blocks
