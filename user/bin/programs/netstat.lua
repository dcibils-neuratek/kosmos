-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- What has gone in and out of this machine, and over what.
--
--   netstat         the totals since the network started, and every
--                   connection with the program that opened it
--
-- The network server's own counts (`NET_OP_STATS`, `docs/netmonitor.md`
-- N1): every frame in and out, what the stack had no room for, and for each
-- connection its bytes each way and the process `SYS_SENDER` named when it
-- was opened. Running totals, as the server keeps them; Network Monitor is
-- what turns two readings into a rate.

local s, why = fs.net_stats("/Network")

if not s then
  print("netstat: " .. tostring(why))
  return
end

local names = {}

for _, p in ipairs(sys.processes() or {}) do names[p.id] = p.name end

local function addr(a)
  return #(a or "") == 4 and ("%d.%d.%d.%d"):format(a:byte(1, 4)) or "?"
end

print(("netstat: in %d bytes in %d frames, out %d bytes in %d frames"):format(
      s.bytes_in, s.frames_in, s.bytes_out, s.frames_out))
print(("netstat: dropped %d in and %d out by the stack"):format(s.dropped_in, s.dropped_out))
print(("netstat: %d connections%s"):format(s.there,
      s.there > #s.connections and (", %d shown"):format(#s.connections) or ""))

for _, c in ipairs(s.connections) do
  local where = c.kind == "tcp" and ("%s:%d"):format(addr(c.remote), c.remote_port) or "-"

  print(("netstat: %-8s %-7s port %-5d to %-21s by %d %s, in %d out %d"):format(
        c.kind, c.state, c.local_port, where, c.process, names[c.process] or "(ended)",
        c.bytes_in, c.bytes_out))
end
