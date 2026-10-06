-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Shares over the network, as a window shows them (`docs/sharing.md`, N6).
--
-- smbfs says what each server is doing (`fs.share_status`), `/Network`
-- lists the servers whose shares were connected and `/Network/<server>`
-- their shares, and Connect to Server remembers the ones a person signed
-- into. **This puts those together into what Tracker's Network group, its
-- status line and its gone-away banner say**, and what Connect to Server
-- reads an address as - so the decisions are in one place, and tested on
-- the build machine (`tools/test_netshares.lua`), where a test costs no
-- boot, as `places.lua`'s are.
--
-- Nothing here asks anything: every function is handed what was heard.
-- Asking is the window's, on its own clock, never in a paint or a press
-- (`CLAUDE.md`, *nothing on the desktop waits on a server*).

local netshares = {}

netshares.ROOT = "/Network"

-- Where the servers a person signed into are remembered: a table of the
-- settings kit's, `/Home/Preferences/sharing` - addresses, shares and
-- accounts, **never a password** (the keyring is step N8).
netshares.PREFS = "sharing"
netshares.RECENT_MOST = 8

--
-- **An address as a person types it**: `smb://192.168.1.38/Projects`,
-- `192.168.1.38`, `diego-mac`, `10.0.2.2:4450/Projects/inside` - the server
-- with its port as typed, the share after the first slash, and anything
-- after that the folder within it. Nil for nothing at all.
--
function netshares.split(text)
  local rest = tostring(text or ""):match("^%s*(.-)%s*$")

  rest = rest:gsub("^[Ss][Mm][Bb]://", "")

  local server, after = rest:match("^([^/]+)/*(.*)$")

  if not server then return nil end

  after = after:gsub("/+$", "")

  local share, within = after:match("^([^/]*)/*(.*)$")

  return server, share or "", within or ""
end

-- And back: `smb://server/share`, or `smb://server` with no share.
function netshares.url(address, share)
  return "smb://" .. tostring(address)
         .. ((share and share ~= "") and ("/" .. share) or "")
end

--
-- **What smbfs calls a server in `/Network`**: the name it gives itself,
-- or that name and its address when two give the same one - whichever of
-- those the listing holds, in the listing's own spelling. Nil when the
-- server is not in `/Network`, which is a server never connected.
--
function netshares.label(rec, listing)
  local name = (rec.name and rec.name ~= "") and rec.name or rec.address
  local both = name .. " (" .. tostring(rec.address) .. ")"

  for _, n in ipairs(listing or {}) do
    if n:lower() == name:lower() then return n end
  end

  for _, n in ipairs(listing or {}) do
    if n:lower() == both:lower() then return n end
  end

  return nil
end

--
-- **The servers of the Network group**, in the order smbfs was asked for
-- them and then the remembered ones nobody has signed into since: each
-- `{ address, name, label, path, state, dot, lock, note, shares, rec,
-- remembered }`.
--
--   `status`    fs.share_status()'s list
--   `listing`   fs.list("/Network")
--   `shares_of` a function from a label to fs.list("/Network/<label>")
--   `recent`    what Connect to Server remembered
--
-- `dot` is the drawing's: "live" connected, "warn" away, "none" being
-- asked; a server refused, or remembered and not signed into, is `lock`ed
-- - the drawing's "seen, not signed in", whose page offers Sign in.
--
function netshares.servers(status, listing, shares_of, recent)
  local out, seen = {}, {}

  for _, rec in ipairs(status or {}) do
    if not rec.probe and not seen[rec.address] then
      seen[rec.address] = true

      local label = netshares.label(rec, listing)
      local one = {
        address = rec.address, rec = rec, label = label,
        name = label or ((rec.name ~= "" and rec.name) or rec.address),
        state = rec.state, shares = {},
      }

      if label then
        one.path = netshares.ROOT .. "/" .. label

        for _, share in ipairs((shares_of and shares_of(label)) or {}) do
          one.shares[#one.shares + 1] = { name = share,
                                          path = one.path .. "/" .. share }
        end
      end

      if rec.state == "connected" then
        one.dot = "live"
        if #one.shares == 0 then one.note = "no shares" end
      elseif rec.state == "away" and label then
        one.dot = "warn"
      elseif rec.state == "asking" or rec.state == "answered" then
        one.dot = "none"
      else
        one.lock = true
      end

      out[#out + 1] = one
    end
  end

  for _, r in ipairs(recent or {}) do
    if r.address and not seen[r.address] then
      seen[r.address] = true
      out[#out + 1] = { address = r.address, name = r.address, lock = true,
                        state = "remembered", shares = {}, remembered = r }
    end
  end

  return out
end

--
-- **The rows `ui.sidebar` draws for them**: the group's name with its
-- Connect link, each server and its shares under it, and "All of the
-- network" - `docs/sharing.html`'s Network group. A server's id is
-- `#server:` and its address, so the window finds the record again; a
-- share's id is its path, as every place's is.
--
function netshares.rows(servers)
  local rows = {
    { heading = true, id = "#network", name = "Network",
      action = "Connect\u{2026}" },
  }

  for _, s in ipairs(servers or {}) do
    rows[#rows + 1] = {
      id = "#server:" .. s.address, name = s.name, icon = "server",
      path = s.path, dot = s.dot, lock = s.lock, note = s.note,
      dim = (s.dot == "warn") or nil, server = s, again = true,
    }

    for _, share in ipairs(s.shares) do
      rows[#rows + 1] = { id = share.path, name = share.name, icon = "folder",
                          path = share.path, indent = true, server = s,
                          again = true }
    end
  end

  rows[#rows + 1] = { id = netshares.ROOT, name = "All of the network",
                      icon = "globe", path = netshares.ROOT, accent = true }

  return rows
end

--
-- **Which server, which share and what within it a path is** - `/Network/
-- MACPEER/Projects/inside` is "MACPEER", "Projects", "inside" - or nil for
-- a path not under `/Network`. `/Network` itself is "", "", "".
--
function netshares.under(path)
  local rest = tostring(path or ""):match("^/[Nn][Ee][Tt][Ww][Oo][Rr][Kk](.*)$")

  if not rest or (rest ~= "" and rest:sub(1, 1) ~= "/") then return nil end

  local server, share, within = rest:match("^/([^/]*)/?([^/]*)/?(.*)$")

  return server or "", share or "", within or ""
end

-- The server record a label names, of `netshares.servers`'s list.
function netshares.find(servers, label)
  for _, s in ipairs(servers or {}) do
    if s.label and label and s.label:lower() == label:lower() then return s end
  end

  return nil
end

--
-- **The trail, as the drawing writes it**: `Network › MACPEER › Projects`,
-- the whole of it, since a share's place is the server as much as the
-- folder - and the innermost two when deeper, with an ellipsis before.
--
function netshares.trail(path)
  local server, share, within = netshares.under(path)

  if not server then return nil end

  local parts = { "Network" }

  if server ~= "" then parts[#parts + 1] = server end
  if share ~= "" then parts[#parts + 1] = share end

  for part in within:gmatch("[^/]+") do parts[#parts + 1] = part end

  if #parts > 3 then
    parts = { "\u{2026}", parts[#parts - 1], parts[#parts] }
  end

  return table.concat(parts, " \u{203a} ")
end

--
-- **What a share's status line says of its server** (`docs/sharing.html`):
-- "MACPEER · SMB 3.1.1, signed · as kosmos" - sealed is the drawing's
-- "encrypted" - and, while bytes are arriving, how fast.
--
function netshares.status_line(rec, rate)
  if not rec then return "" end

  local name = (rec.name ~= "" and rec.name) or rec.address
  local how = "SMB " .. tostring(rec.dialect or "?")

  if rec.sealing then
    how = how .. ", signed and encrypted"
  elseif rec.signing then
    how = how .. ", signed"
  end

  local words = ("%s \u{b7} %s \u{b7} as %s"):format(name, how,
                                                     tostring(rec.account))

  if rate and rate > 0 then
    words = words .. " \u{b7} " .. netshares.rate(rate) .. " arriving"
  end

  return words
end

-- Bytes a second, as a person reads them.
function netshares.rate(bytes)
  if bytes >= 1048576 then
    return ("%.1f MB/s"):format(bytes / 1048576)
  elseif bytes >= 1024 then
    return ("%d KB/s"):format(bytes // 1024)
  end

  return ("%d B/s"):format(bytes)
end

--
-- **The banner of a server gone away** (`docs/sharing.html`, *Gone away*):
-- its title - "MACPEER is not answering - retrying", or "trying again
-- now" - and the sentence under it, from what `share_status` said: since
-- when, by the clock on the wall (`since`, already in words), and when the
-- next try is. Nothing here spins: it is words that change when smbfs's
-- answer does.
--
function netshares.away_words(rec, since)
  local name = (rec.name ~= "" and rec.name) or rec.address
  local title = name .. " is not answering - "
                .. (rec.trying and "trying again now" or "retrying")
  local parts = {}

  parts[#parts + 1] = since and ("Nothing since " .. since .. ".")
                      or "Nothing heard from it."

  if rec.trying then
    parts[#parts + 1] = "Trying now."
  elseif (rec.next_try_ms or 0) > 0 then
    parts[#parts + 1] = ("The next try is in %d s."):format((rec.next_try_ms + 999) // 1000)
  end

  parts[#parts + 1] = "What is below is the folder as it was then."

  return title, table.concat(parts, " ")
end

--
-- **The servers Connect to Server remembers**, newest first: `entry` -
-- `{ address, share, account, at }` - put at the front, any earlier one of
-- the same address and share taken out, and no more than `most` kept.
--
function netshares.remember(list, entry, most)
  local out = { entry }

  for _, r in ipairs(list or {}) do
    if not (r.address == entry.address and (r.share or "") == (entry.share or "")) then
      out[#out + 1] = r
    end
  end

  while #out > (most or netshares.RECENT_MOST) do table.remove(out) end

  return out
end

-- Forget: every entry of that address.
function netshares.forget(list, address)
  local out = {}

  for _, r in ipairs(list or {}) do
    if r.address ~= address then out[#out + 1] = r end
  end

  return out
end

return netshares
