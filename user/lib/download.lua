-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- **A file downloaded to a disk, of any size** (`docs/xavier.md`, *Models*):
-- the bytes go from the connection into a region, from the region onto the
-- disk, and through a SHA-256 on the way, all in C - so a model of 1.6 GB is
-- the same to this as a page of text. Models is the first user; Packages and
-- the browser's downloads are the next.
--
--   local download = use("/Kosmos/Libraries/download.lua")
--   local ok, why = download.file("https://huggingface.co/x/resolve/main/m.gguf",
--                                 "/Home/Models/m.gguf",
--                                 { sha256 = "7607...", progress = function(got, total) end })
--
-- - **Redirects followed**, to another host or port: Hugging Face hands a
--   file's address to its store with a 302, and the signature in that
--   address is only good for a while, so it is asked for each time.
-- - **Resumed** where a download was cut: a file already there is kept, its
--   bytes summed first, and the rest asked for with a Range. A server that
--   answers 200 rather than 206 sends the whole of it again, and the file is
--   begun again with it.
-- - **Held to its SHA-256** when one is given, before it is called done: a
--   file whose sum is wrong is left where it is, said so, and not used.
--
-- - **Given up on when it stops hearing**: nothing for `QUIET_S` seconds -
--   no byte, no step - and the connection is closed and said so, the bytes
--   so far kept, so asking again goes on from them. Three downloads on the
--   M700 each held a connection to the CDN open with nothing arriving and
--   never ended (9 November).
--
-- `opts`: `sha256` the sum to hold it to; `size` the size it should be;
-- `quiet` the seconds of nothing to give up after, `QUIET_S` when not given;
-- `progress(got, total)` told as pieces land; `name` the name to hold a
-- certificate to, for an address given by number (`fetch --name`);
-- `anchors` more authorities in DER, besides the person's own.
--
-- What comes back is the size and the sum, or nil and why - and "wrong"
-- third when the file is whole and not what it should be, which going on
-- would not mend: the caller decides whether to keep it.

local http = use("/Kosmos/Libraries/http.lua")
local netstream = use("/Kosmos/Libraries/netstream.lua")
local regions = use("/Kosmos/Libraries/regions.lua")
local crypto = use("/Kosmos/Kits/crypto")

local download = {}

download.PIECE = 4 * 1024 * 1024          -- a region's worth, written as one transaction
download.REDIRECTS = 5
download.QUIET_S = 30                      -- seconds of nothing before giving up

-- The counter's rate, for measuring how long nothing was heard: read once.
local counter_hz

local piece                                -- the region, made once and kept

-- An address that a redirect gave, made whole against the one it came from.
local function resolved(location, parts)
  if location:match("^%a[%w+.%-]*://") then return location end

  if location:sub(1, 2) == "//" then return parts.scheme .. ":" .. location end

  if location:sub(1, 1) == "/" then
    return parts.scheme .. "://" .. parts.hostport .. location
  end

  return parts.scheme .. "://" .. parts.hostport .. parts.path:gsub("[^/]*$", "") .. location
end

-- What is already on the disk, summed: the hasher, and how many bytes.
local function sum_existing(path, size, hash)
  local at = 0

  while at < size do
    local want = math.min(download.PIECE, size - at)
    local got, why = fs.read_into(path, piece.cap, at, want)

    if got ~= want then
      return nil, ("%s could not be read at %d: %s"):format(path, at, tostring(why))
    end

    hash:update(piece.at, want)
    at = at + want
  end

  return at
end

-- One GET of `address` from byte `from`: "done", or "redirect" and where,
-- or nil and why. Pieces are written at their offsets as they come.
local function one(address, path, from, hash, opts, state)
  local parts, bad = http.split(address)

  if not parts then return nil, bad end

  local where, why, said = netstream.address(parts.host)

  if not where then
    return nil, ("cannot look up %s: %s"):format(parts.host, said or tostring(why))
  end

  local anchors = netstream.trusted()

  for _, der in ipairs(opts.anchors or {}) do anchors[#anchors + 1] = der end

  local d, refused = fs.download("/Network", {
    address = where, port = parts.port, host = opts.name or parts.host, path = parts.path,
    tls = parts.scheme == "https", from = from, at = piece.at, bytes = piece.size,
    anchors = anchors,
  })

  if not d then return nil, ("%s: %s"):format(parts.hostport, tostring(refused)) end

  local at, began = from, false

  -- When something last moved: a byte arrived, or the step changed.
  counter_hz = counter_hz or (fs.read("/Devices/cpu") or {}).counter_hz or 62500000
  local quiet = (tonumber(opts.quiet) or download.QUIET_S) * counter_hz
  local heard, last_got, last_step = sys.ticks(), -1, nil

  local function write_out()
    local n = d:ready()

    if n == 0 then return true end

    local put, oops = fs.write_from_at(path, piece.cap, at, n, true)

    if put ~= n then
      return nil, ("%s could not be written at %d: %s"):format(path, at, tostring(oops))
    end

    hash:update(piece.at, n)
    d:taken()
    at = at + n

    if opts.progress then opts.progress(at, state.total) end

    return true
  end

  while true do
    local step, failed = d:step()

    if step == "failed" then
      local ok, oops = write_out()
      d:close()
      return nil, ok and failed or oops, at
    end

    -- The head is in: where it goes, how large the whole is, and whether
    -- the server took the Range.
    if not began and (step == "body" or step == "done") then
      began = true
      local status = d:status()

      if status >= 300 and status < 400 then
        local location = d:header("location")
        d:close()

        if not location then return nil, ("a %d with nowhere to go"):format(status) end

        return "redirect", resolved(location, parts)
      end

      if status ~= 200 and status ~= 206 then
        local words = d:body():sub(1, 120):gsub("%s+", " ")
        d:close()
        return nil, ("%s answered %d%s"):format(parts.hostport, status,
                                                words ~= "" and (": " .. words) or "")
      end

      if status == 206 then
        local total = tonumber((d:header("content-range") or ""):match("/(%d+)$"))

        state.total = total or (d:length() >= 0 and from + d:length() or nil)
      else
        -- The whole file, from its first byte, whatever was asked.
        if from > 0 then
          local made, cannot = fs.size(path, 0, true)

          if not made then
            d:close()
            return nil, ("%s could not be begun again: %s"):format(path, tostring(cannot))
          end

          hash = crypto.sha256_begin()
          state.hash = hash
          at = 0
        end

        state.total = d:length() >= 0 and d:length() or nil
      end
    end

    local ok, oops = write_out()

    if not ok then
      d:close()
      return nil, oops
    end

    if step == "done" then
      d:close()
      state.size = at
      return "done"
    end

    local now = sys.ticks()

    if d:got() ~= last_got or step ~= last_step then
      heard, last_got, last_step = now, d:got(), step
    elseif now - heard > quiet then
      d:close()
      return nil, ("nothing heard from %s for %d s"):format(parts.hostport,
                                                            (now - heard) // counter_hz), at
    end

    -- Nothing moved: wait on the connection rather than spin. A full
    -- region was emptied above, so the next step has room.
    d:wait(step == "sending" and 10 or 50)
  end
end

function download.file(address, path, opts)
  opts = opts or {}

  if not piece then
    local why
    piece, why = regions.make(download.PIECE)

    if not piece then return nil, why end
  end

  -- What a cut download left: kept, and summed.
  local hash = crypto.sha256_begin()
  local have = (fs.getattr(path) or {}).size or 0

  if opts.size and have > opts.size then
    fs.size(path, 0, true)
    have = 0
  end

  if have > 0 then
    local summed, why = sum_existing(path, have, hash)

    if not summed then return nil, why end
  end

  local state = { hash = hash, total = opts.size }

  -- Already whole: only the sum to check.
  if opts.size and have == opts.size then
    state.size = have
  else
    local next_address = address

    for _ = 0, download.REDIRECTS do
      local how, more = one(next_address, path, have, state.hash, opts, state)

      if not how then return nil, more end

      if how == "done" then break end

      next_address = more
      state.redirected = (state.redirected or 0) + 1

      if state.redirected > download.REDIRECTS then
        return nil, "more than " .. download.REDIRECTS .. " redirects"
      end
    end
  end

  local sum = state.hash:hex()

  if opts.size and state.size ~= opts.size then
    return nil, ("%s is %d bytes, not the %d it should be"):format(path, state.size or 0, opts.size), "wrong"
  end

  if opts.sha256 and sum ~= opts.sha256:lower() then
    return nil, ("%s's SHA-256 is %s, not the %s it should be"):format(path, sum, opts.sha256:lower()), "wrong"
  end

  return state.size, sum
end

return download
