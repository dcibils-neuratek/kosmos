-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: needs network
-- Puts one Ethernet frame on the wire, and says what came back.
--
--   netframe            one broadcast frame, then listen briefly
--   netframe 20         listen for twenty passes instead
--
-- **This is the driver's test and not a network tool.** There is no address
-- here, no protocol and no checksum - it sends a frame with a made-up
-- EtherType that nothing will answer, because what is being established is
-- that the card takes bytes and puts them somewhere a host can see. The
-- witness is on the other side: QEMU's `filter-dump` writes a pcap and
-- `tools/run_network.py` reads it. Nothing inside the machine can tell you
-- a frame left it.
--
-- `ping` is the program that means something to a person, and it needs a
-- stack. This one needs the card and nothing else, which is what makes it
-- the right thing to have first: when ping does not work, this says whether
-- the card is the reason.

local card = sys.net()

if not card then
  print("netframe: this machine has no network card")
  return
end

local function hex(s)
  local out = {}

  for i = 1, #s do out[i] = ("%02x"):format(s:byte(i)) end

  return table.concat(out, ":")
end

print(("card %s, MTU %d"):format(hex(card.mac), card.mtu))

--
-- The frame. Fourteen bytes of Ethernet header and then something to look
-- at in a capture.
--
-- Broadcast, because there is nobody in particular to talk to and a
-- broadcast is the one destination that is always correct. EtherType 0x88b5
-- is the block IEEE reserves for exactly this - local experimental use - so
-- this is not pretending to be a protocol somebody else defined.
--
local BROADCAST = "\255\255\255\255\255\255"
local ETHERTYPE = "\136\181"                  -- 0x88b5
local BODY      = "kosmos says hello from the other side of a virtqueue"

local frame = BROADCAST .. card.mac .. ETHERTYPE .. BODY

print(("sending %d bytes"):format(#frame))

if not sys.net_send(frame) then
  print("netframe: the card would not take it")
  return
end

print("sent")

--
-- And whatever arrives, for a moment.
--
-- Under QEMU's user-mode networking this is usually nothing: slirp is a
-- NAT rather than a wire, so a broadcast nobody is on goes nowhere and
-- there is no other machine to hear it. Printed anyway, because on a real
-- network there would be, and an empty answer here is a fact rather than a
-- failure.
--
local passes = tonumber((args or ""):match("%d+") or "") or 10
local seen = 0

for _ = 1, passes do
  local frame_in = sys.net_recv()

  while frame_in do
    seen = seen + 1

    print(("received %d bytes, from %s, type 0x%02x%02x")
          :format(#frame_in, hex(frame_in:sub(7, 12)),
                  frame_in:byte(13) or 0, frame_in:byte(14) or 0))

    frame_in = sys.net_recv()
  end

  sys.sleep(5)
end

print(("%d frame%s arrived"):format(seen, seen == 1 and "" or "s"))
