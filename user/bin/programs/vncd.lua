-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: needs network desktop
-- vncd: this machine's screen, to a VNC viewer on another.
--
--   vncd            on port 5900
--   vncd 5901       on that one
--
-- From the Mac: any VNC viewer - TigerVNC, RealVNC, the Mac's own Screen
-- Sharing at `vnc://<address>` - pointed at the machine.
--
-- **The Screen page of Servers** (`docs/servers.html`; `roadmap.md` remote
-- 7) - Diego, 29 September: "We can do a vnc server after Telnetd so we can
-- remote access the desktop with a simple vnc client". The screen, looked
-- at (7a), and used - its pointer and keys - when the Servers window says a
-- viewer may (7b); only looked at otherwise, the drawing's default.
--
-- **The desktop is lent, not reached.** The window manager mounts
-- `/Running/wm/remote` in a program it launches whose header says `needs
-- desktop`, and nowhere else - so this is started by the desktop: the
-- Servers window's switch, `open vncd`, or the window manager itself when
-- the window keeps it to start with the machine.
--
-- **The screen is asked of the window manager, never read from the
-- framebuffer.** It owns every pixel; this hands it a region the screen's
-- size (`watch`), and after each pass it copies there what it composed and
-- keeps the rectangles, which `watched` hands back. Control by message,
-- data by shared memory. Nothing is copied while nobody is connected: the
-- window manager lets the region go when it is not asked for five seconds,
-- and this watches again when a viewer comes.
--
-- **A viewer's keys become what a keyboard's would**: the key's code for
-- the event, and the characters the kernel's own table and xterm's
-- sequences make of it, so the desktop cannot tell them apart.
--
-- **RFB 3.3**, the version every viewer speaks, and in which the server
-- chooses the security: none, or VNC Authentication when the Servers
-- window has a password - read at each connection, so a password set there
-- holds for the next viewer without a restart. VNC Authentication is DES
-- over a random challenge, both in C (`/Kosmos/Kits/crypto`), and it is
-- weak: eight characters and a cipher broken since 1998. It keeps out a
-- visitor on the network, which is what it is for here.
--
-- **ZRLE when the viewer offers it, raw pixels when it does not**, both in
-- the pixel format the viewer asks for (`roadmap.md`, remote 7c). Raw is
-- the format packed in C (`surface:pack`): ten megabytes a frame at
-- 1720x1440, which the M700 sent at 1.4 MB a second and Diego, reaching it
-- from outside the house on 7 October, found "really slow". ZRLE is the
-- rectangle as tiles of 64 by 64 - one colour, a small palette, runs - in
-- C (`surface:zrle`, `gfx/zrle.c`), deflated on one zlib stream a viewer
-- (`compress.zstream`), and a desktop comes to a few per cent of its
-- pixels. Every viewer that matters speaks it: TigerVNC, RealVNC, macOS's
-- Screen Sharing. And the subnet rule `telnetd` keeps: a viewer from
-- outside this machine's own network is closed unanswered.

local crypto = use("/Kosmos/Kits/crypto")
local compress = use("/Kosmos/Kits/compress")
local regions = use("/Kosmos/Libraries/regions.lua")

-- ZRLE's number for itself in SetEncodings and on each rectangle, and the
-- rows a rectangle is cut into: one row of tiles, so each band's tiles are
-- whole and the region they are written into stays small.
local ZRLE = 16
local BAND = 64

local words = {}

for w in tostring(args or ""):gmatch("%S+") do words[#words + 1] = w end

local port = tonumber(words[1]) or 5900
local prefs = use("/Kosmos/Libraries/prefs.lua")
local ipv4 = use("/Kosmos/Libraries/ipv4.lua")
local wmproto = use("/Kosmos/Libraries/wmproto.lua")

--
-- What every program that serves the network begins with, from
-- `netprogram.lua`, as `telnetd` begins: a card, the port, its folder in
-- `/Temporary` for the Servers window, a name in `/Running` for that
-- window's Disconnect, and only this machine's own subnet let in.
--
local net = use("/Kosmos/Libraries/netprogram.lua").open{
  name = "vncd", port = port, named = true, neighbours = true,
}

if not net then return end

print(("vncd: on port %d, at %s"):format(port, net:address()))

-- **Keys and the pointer lent by the boot command line** (`roadmap.md`,
-- build, boot and test the M700 in a loop; Diego, 3 October, choosing it
-- over the Servers window's switch): `opt/kosmos/vnc=control` lends them to
-- every viewer, whatever the window says. It grants nothing new - the words
-- come from whoever started the machine, which on a network boot is the
-- server that also handed it its kernel - and it survives every restart,
-- which a setting in a `/Home` held in memory does not. A machine started
-- from its own stick has it only if the stick's line says so. Said here, so
-- the log shows why a viewer could type.
local LENT_BY_BOOT = sys.boot("opt/kosmos/vnc") == "control"

if LENT_BY_BOOT then
  print("vncd: keys and the pointer lent to every viewer, as opt/kosmos/vnc asks")
end

local TICK_HZ = (sys.info() or {}).tick_hz or 250

--------------------------------------------------------------------------
-- What the Servers window reads: `/Temporary/vncd`, as `httpd` and
-- `telnetd` keep theirs, through the same library.
--------------------------------------------------------------------------

local viewers = {}

local function publish()
  local list = {}

  for _, v in ipairs(viewers) do
    if v.stage == "normal" then
      list[#list + 1] = { from = ipv4.text(v.from), bpp = v.format.bpp }
    end
  end

  net:publish{ viewers = list }
end

--------------------------------------------------------------------------
-- The screen, as the window manager hands it over (`wmproto.lua`, as
-- `screenshot` asks for it).
--------------------------------------------------------------------------

local screen = nil        -- { w, h, cap, surface, watching }

local function watch()
  if not screen then
    local why, lent

    screen, why, lent = wmproto.screen()

    if not screen and not lent then
      return nil, why .. " - this is started from the desktop: the Servers "
                  .. "window, or open vncd"
    end

    if not screen then return nil, why end
  end

  if not screen.watching then
    local ok, why = wmproto.watch(screen)

    if not ok then return nil, why end
  end

  return screen
end

--------------------------------------------------------------------------
-- RFB: the server's half.
--------------------------------------------------------------------------

-- This surface's own pixel: 32 bits, little-endian, red at 16.
local NATIVE = { bpp = 32, depth = 24, big = false, rmax = 255, gmax = 255,
                 bmax = 255, rshift = 16, gshift = 8, bshift = 0 }

local function format_bytes(f)
  return string.pack(">BBBBI2I2I2BBBxxx", f.bpp, f.depth, f.big and 1 or 0, 1,
                     f.rmax, f.gmax, f.bmax, f.rshift, f.gshift, f.bshift)
end

-- What the Servers window says, read afresh for each viewer: its password,
-- and whether it may use the keyboard and the pointer.
local function settings()
  local all = prefs.read("servers")
  local vnc = type(all) == "table" and type(all.vnc) == "table" and all.vnc or {}
  local p = vnc.password

  return (type(p) == "string" and p ~= "") and p:sub(1, 8) or nil,
         LENT_BY_BOOT or vnc.control == true
end

--------------------------------------------------------------------------
-- A viewer's keys, as a keyboard's.
--
-- An X keysym - which is what RFB sends - into the key's code, for the
-- event, and the characters it makes: for a printable key the keysym is its
-- character already, the viewer having applied Shift; otherwise the
-- kernel's own sequences (`hal/keys.c`) - xterm's, with a modifier number
-- of 1 plus 1 for Shift, 2 for Alt and 4 for Control - and Control with a
-- letter its control character. The US layout, as the kernel's table is.
--------------------------------------------------------------------------

local CODE_OF = {}

do
  local rows = {
    { "1234567890-=", "!@#$%^&*()_+", 2 },
    { "qwertyuiop[]", "QWERTYUIOP{}", 16 },
    { "asdfghjkl;'`", 'ASDFGHJKL:"~', 30 },
    { "zxcvbnm,./", "ZXCVBNM<>?", 44 },
  }

  for _, r in ipairs(rows) do
    for i = 1, #r[1] do
      CODE_OF[r[1]:byte(i)] = r[3] + i - 1
      CODE_OF[r[2]:byte(i)] = r[3] + i - 1
    end
  end

  CODE_OF[("\\"):byte(1)], CODE_OF[("|"):byte(1)], CODE_OF[32] = 43, 43, 57
end

-- A key that is not a character: its code, and its character or its
-- sequence (`n`, the number before the final, and `final`; `ss3` for F1-F4).
local SPECIAL = {
  [0xff08] = { code = 14, char = 8 },   [0xff09] = { code = 15, char = 9 },
  [0xff0d] = { code = 28, char = 10 },  [0xff8d] = { code = 28, char = 10 },
  [0xff1b] = { code = 1, char = 27 },
  [0xff50] = { code = 102, n = 0, final = "H" }, [0xff57] = { code = 107, n = 0, final = "F" },
  [0xff51] = { code = 105, n = 0, final = "D" }, [0xff52] = { code = 103, n = 0, final = "A" },
  [0xff53] = { code = 106, n = 0, final = "C" }, [0xff54] = { code = 108, n = 0, final = "B" },
  [0xff55] = { code = 104, n = 5, final = "~" }, [0xff56] = { code = 109, n = 6, final = "~" },
  [0xff63] = { code = 110, n = 2, final = "~" }, [0xffff] = { code = 111, n = 3, final = "~" },
  [0xffbe] = { code = 59, n = 0, final = "P", ss3 = true },
  [0xffbf] = { code = 60, n = 0, final = "Q", ss3 = true },
  [0xffc0] = { code = 61, n = 0, final = "R", ss3 = true },
  [0xffc1] = { code = 62, n = 0, final = "S", ss3 = true },
  [0xffc2] = { code = 63, n = 15, final = "~" }, [0xffc3] = { code = 64, n = 17, final = "~" },
  [0xffc4] = { code = 65, n = 18, final = "~" }, [0xffc5] = { code = 66, n = 19, final = "~" },
  [0xffc6] = { code = 67, n = 20, final = "~" }, [0xffc7] = { code = 68, n = 21, final = "~" },
  [0xffc8] = { code = 87, n = 23, final = "~" }, [0xffc9] = { code = 88, n = 24, final = "~" },
}

-- The modifiers, by keysym: their codes, and which one each is.
local MODIFIER = {
  [0xffe1] = { 42, "shift" }, [0xffe2] = { 54, "shift" },
  [0xffe3] = { 29, "ctrl" },  [0xffe4] = { 97, "ctrl" },
  [0xffe9] = { 56, "alt" },   [0xffea] = { 100, "alt" },
  [0xffeb] = { 125, "super" }, [0xffec] = { 126, "super" },
  [0xffe7] = { 125, "super" }, [0xffe8] = { 126, "super" },   -- Meta: a Mac's Command
  [0xffe5] = { 58, "caps" },
}

-- `ESC [ n ; m final`, as `sequence_of` in `hal/keys.c` writes it.
local function sequence(n, m, final, ss3)
  if ss3 and m == 1 then return "\27O" .. final end
  if n == 0 and m > 1 then n = 1 end

  return "\27[" .. (n > 0 and tostring(n) or "") .. (m > 1 and (";" .. m) or "") .. final
end

-- The event and the characters for one key, down or up; `held` is the
-- viewer's own modifiers, kept here since the viewer sends them as keys.
local function translate(held, keysym, down)
  local mod = MODIFIER[keysym]

  if mod then
    local was = held[mod[2]]

    if mod[2] == "caps" then
      if down then held.caps = not held.caps end
    else
      held[mod[2]] = down
    end

    -- Super tapped alone, released with nothing pressed while it was held,
    -- is its own sequence: the kernel's, which opens the menu.
    if mod[2] == "super" then
      if down then held.super_used = false
      elseif was and not held.super_used then return mod[1], "\27[1;9~" end
    end

    return mod[1], ""
  end

  if held.super then held.super_used = true end

  local m = 1 + (held.shift and 1 or 0) + (held.alt and 2 or 0) + (held.ctrl and 4 or 0)
  local special = SPECIAL[keysym]

  if special then
    if not down then return special.code, "" end

    if special.final then return special.code, sequence(special.n, m, special.final, special.ss3) end
    if keysym == 0xff09 and held.shift and not held.ctrl then return special.code, "\27[Z" end

    return special.code, string.char(special.char)
  end

  if keysym < 0x20 or keysym > 0x7e then return nil end

  local code = CODE_OF[keysym]

  if not down then return code, "" end

  local c = keysym

  if held.caps and c >= 0x61 and c <= 0x7a and not held.shift then c = c - 32 end

  if held.super then return code, "\27[1;9" .. string.char(c) end

  if held.ctrl then
    local lower = (c >= 0x41 and c <= 0x5a) and c + 32 or c

    if lower >= 0x61 and lower <= 0x7a then return code, string.char(lower - 0x60) end

    return code, sequence(c, m, "u")
  end

  return code, string.char(c)
end

-- Sixteen bytes a viewer must answer, from the Crypto Kit's generator -
-- which the hardware seeds - so a challenge cannot be guessed and an
-- answer seen once cannot be played again. Nil on a machine with no source,
-- and then no password is asked for at all: a challenge anyone can predict
-- would be a lock that only looks locked.
local function challenge()
  local ok, bytes = pcall(crypto.random, 16)

  return ok and bytes or nil
end

-- VNC's key is the password with each byte's bits reversed - the one quirk
-- of its DES - and the answer is the challenge under it, block by block.
local function expected(pass, chal)
  local key = {}

  for i = 1, 8 do
    local b, r = pass:byte(i) or 0, 0

    for _ = 1, 8 do
      r = (r << 1) | (b & 1)
      b = b >> 1
    end

    key[i] = string.char(r)
  end

  return crypto.des(table.concat(key), chal)
end

local function send(v, bytes)
  v.out[#v.out + 1] = bytes
  v.queued = v.queued + #bytes
end

local function close(at, why)
  local v = viewers[at]

  regions.free(v.tiles)
  v.tiles, v.zstream = nil, nil

  if v.stage == "normal" then
    net:note(ipv4.text(v.from) .. "  " .. (why or "left"))
  end

  v.conn:close()
  table.remove(viewers, at)
  publish()
end

local function open(conn, from)
  local v = { conn = conn, from = from, stage = "version",
              inb = "", out = {}, queued = 0, format = NATIVE, pending = {},
              input = {}, held = {}, mask = 0, control = false }

  viewers[#viewers + 1] = v
  send(v, "RFB 003.003\n")
end

-- A Disconnect from the Servers window: that address's viewers dropped,
-- whatever they were still owed.
local function disconnect(from)
  local ended = 0

  for _, v in ipairs(viewers) do
    if ipv4.text(v.from) == from then
      v.leaving = "disconnected from this machine"
      v.out, v.queued, v.job = {}, 0, nil
      ended = ended + 1
    end
  end

  return ended
end

-- A rectangle list kept short: past sixty-four, one rectangle round them.
local function add_pending(v, x, y, w, h)
  local list = v.pending

  if #list >= 64 then
    local x0, y0, x1, y1 = x, y, x + w, y + h

    for _, r in ipairs(list) do
      x0, y0 = math.min(x0, r[1]), math.min(y0, r[2])
      x1, y1 = math.max(x1, r[1] + r[3]), math.max(y1, r[2] + r[4])
    end

    v.pending = { { x0, y0, x1 - x0, y1 - y0 } }
  else
    list[#list + 1] = { x, y, w, h }
  end
end

-- The part of `r` inside the request's rectangle, or nil.
local function inside(r, want)
  local x0, y0 = math.max(r[1], want.x), math.max(r[2], want.y)
  local x1 = math.min(r[1] + r[3], want.x + want.w)
  local y1 = math.min(r[2] + r[4], want.y + want.h)

  if x1 <= x0 or y1 <= y0 then return nil end

  return { x0, y0, x1 - x0, y1 - y0 }
end

-- An update begun: the header now, the pixels as the connection takes them.
--
-- For ZRLE each rectangle is cut into bands a row of tiles high first, since
-- the header says how many rectangles follow and each band is one.
local function begin_update(v, rects)
  if v.zrle then
    local bands = {}

    for _, r in ipairs(rects) do
      for top = 0, r[4] - 1, BAND do
        bands[#bands + 1] = { r[1], r[2] + top, r[3], math.min(BAND, r[4] - top) }
      end
    end

    rects = bands
  end

  v.job = { rects = rects, i = 1, row = 0 }
  send(v, string.pack(">BxI2", 0, #rects))
end

-- One band as ZRLE: its tiles written into the viewer's region, deflated on
-- its stream, and sent as the length and the bytes. The region is made the
-- first time, for a band as wide as the screen; the stream lives as long as
-- the connection, because the viewer's inflater does.
local function send_zrle(v, x, y, w, h)
  if not v.tiles then
    local bytes = gfx.zrle_bound(screen.w, BAND)

    v.tiles = assert(regions.make(bytes))
    v.tiles_cap = bytes
    v.zstream = assert(compress.zstream(6))
  end

  local n = assert(screen.surface:zrle(x, y, w, h, v.format, v.tiles.at, v.tiles_cap))
  local data = v.zstream:deflate(v.tiles.at, n)

  send(v, string.pack(">I2I2I2I2i4I4", x, y, w, h, ZRLE, #data))
  send(v, data)
  v.sent_zrle = (v.sent_zrle or 0) + 16 + #data
  v.sent_pixels = (v.sent_pixels or 0) + w * h * (v.format.bpp // 8)
end

-- Some more of the update in progress, about 16 KB of it: a row band of a
-- rectangle, packed in C, so no string is ever a frame's size.
local function pump(v)
  local job = v.job

  while job and v.queued < 65536 do
    local r = job.rects[job.i]
    local x, y, w, h = r[1], r[2], r[3], r[4]

    if v.zrle then
      -- A band a pass, whole: its tiles are a few kilobytes deflated.
      send_zrle(v, x, y, w, h)
      job.row = h
    else
      if job.row == 0 then
        send(v, string.pack(">I2I2I2I2i4", x, y, w, h, 0))
      end

      local per = w * (v.format.bpp // 8)
      local rows = math.max(1, math.min(h - job.row, 16384 // math.max(1, per)))

      send(v, screen.surface:pack(x, y + job.row, w, rows, v.format))
      job.row = job.row + rows
    end

    if job.row >= h then
      job.i, job.row = job.i + 1, 0

      if job.i > #job.rects then
        v.job = nil
        job = nil
      end
    end
  end
end

-- An update the viewer asked for, if there is one to give.
local function maybe_update(v)
  local want = v.want

  if not want or v.job or not screen or not screen.watching then return end

  local rects = {}

  if not want.incremental then
    local r = inside({ 0, 0, screen.w, screen.h }, want)

    if r then rects[1] = r end
    v.pending = {}
  else
    for _, p in ipairs(v.pending) do
      local r = inside(p, want)

      if r then rects[#rects + 1] = r end
    end

    if #rects == 0 then return end

    v.pending = {}
  end

  v.want = nil

  if #rects > 0 then begin_update(v, rects) end
end

-- What the viewer said, taken as far as it is whole. False to close.
local function take(v)
  while true do
    local inb = v.inb

    if v.stage == "version" then
      if #inb < 12 then return true end

      if not inb:match("^RFB %d%d%d%.%d%d%d\n") then
        return false, "not a VNC viewer"
      end

      v.inb = inb:sub(13)

      local pass, control = settings()

      v.control = control

      if pass and not challenge() then
        net:note(ipv4.text(v.from) .. "  refused: a password is kept and this machine "
             .. "has no randomness to ask it with")
        send(v, string.pack(">I4", 0) .. string.pack(">s4", "no randomness for a challenge"))
        return false, "no randomness"
      end

      if pass then
        v.challenge = challenge()
        v.expect = expected(pass, v.challenge)
        send(v, string.pack(">I4", 2) .. v.challenge)
        v.stage = "auth"
      else
        send(v, string.pack(">I4", 1))
        v.stage = "init"
      end
    elseif v.stage == "auth" then
      if #inb < 16 then return true end

      local answer = inb:sub(1, 16)

      v.inb = inb:sub(17)

      if answer ~= v.expect then
        send(v, string.pack(">I4", 1))
        net:note(ipv4.text(v.from) .. "  refused, the wrong password")
        return false, "the wrong password"
      end

      send(v, string.pack(">I4", 0))
      v.stage = "init"
    elseif v.stage == "init" then
      if #inb < 1 then return true end

      v.inb = inb:sub(2)

      local s, err = watch()

      if not s then
        net:note(ipv4.text(v.from) .. "  refused: " .. err)
        return false, err
      end

      local name = "Kosmos at " .. net:address()

      send(v, string.pack(">I2I2", s.w, s.h) .. format_bytes(NATIVE)
              .. string.pack(">s4", name))
      v.stage = "normal"
      net:note(ipv4.text(v.from) .. "  connected")
      publish()

      --
      -- **Said to whoever is at the machine** (Diego, 7 October: "vnc server
      -- should notify when a new connection is made from a client so the
      -- user knows someone connected"): who, and whether they may use the
      -- keyboard and the pointer or only look - once a session, as a banner;
      -- what stays while they are connected is the Deskbar's status icon
      -- (`roadmap.md`, 7d).
      --
      net:tell_connected(v.from, "Screen shared with " .. ipv4.text(v.from),
                         v.control and "by VNC - it can use the keyboard and the pointer"
                                   or "by VNC - it can see the screen, not use it")
    else
      if #inb < 1 then return true end

      local kind = inb:byte(1)
      local need = ({ [0] = 20, [2] = 4, [3] = 10, [4] = 8, [5] = 6, [6] = 8 })[kind]

      if not need then return false, "a message this does not know: " .. kind end
      if #inb < need then return true end

      if kind == 0 then
        -- SetPixelFormat.
        local bpp, depth, big, truecolour, rmax, gmax, bmax, rs, gs, bs =
          string.unpack(">BBBBI2I2I2BBB", inb, 5)

        if truecolour == 0 or (bpp ~= 8 and bpp ~= 16 and bpp ~= 32) then
          return false, "asks for a colour map, which this does not serve"
        end

        v.format = { bpp = bpp, depth = depth, big = big ~= 0, rmax = rmax,
                     gmax = gmax, bmax = bmax, rshift = rs, gshift = gs,
                     bshift = bs }
        v.inb = inb:sub(21)
        publish()
      elseif kind == 2 then
        -- SetEncodings: ZRLE when it is among them, Raw otherwise - Raw is
        -- always allowed. A viewer may send this again mid-connection; a
        -- stream already begun goes on, since the viewer's inflater has it.
        local n = string.unpack(">I2", inb, 3)

        if #inb < 4 + 4 * n then return true end

        v.zrle = false

        for i = 0, n - 1 do
          if string.unpack(">i4", inb, 5 + 4 * i) == ZRLE then v.zrle = true end
        end

        net:note(ipv4.text(v.from) .. "  " .. (v.zrle and "ZRLE" or "raw pixels"))
        v.inb = inb:sub(5 + 4 * n)
      elseif kind == 3 then
        -- FramebufferUpdateRequest.
        local incremental, x, y, w, h = string.unpack(">BI2I2I2I2", inb, 2)

        v.want = { incremental = incremental ~= 0, x = x, y = y, w = w, h = h }
        v.inb = inb:sub(11)
      elseif kind == 4 then
        -- KeyEvent: a keyboard's key, when this viewer may use it.
        local down, keysym = string.unpack(">Bxx>I4", inb, 2)

        v.inb = inb:sub(9)

        if v.control then
          local code, chars = translate(v.held, keysym, down ~= 0)

          if code or (chars and chars ~= "") then
            v.input[#v.input + 1] = { type = "key", code = code or 0,
                                      down = down ~= 0, chars = chars or "" }
          end
        end
      elseif kind == 5 then
        -- PointerEvent: where, and which buttons - RFB's middle and right
        -- are the other way round from here, and its fourth and fifth are
        -- the wheel, a notch a press.
        local mask, x, y = string.unpack(">BI2I2", inb, 2)

        v.inb = inb:sub(7)

        if v.control then
          local buttons = (mask & 1) | ((mask & 4) ~= 0 and 2 or 0) | ((mask & 2) ~= 0 and 4 or 0)
          local wheel = (((mask & 8) ~= 0 and (v.mask & 8) == 0) and 1 or 0)
                      - (((mask & 16) ~= 0 and (v.mask & 16) == 0) and 1 or 0)
          local last = v.input[#v.input]

          -- A move with nothing else changed replaces the move before it,
          -- so a fast viewer is a position rather than a queue.
          if last and last.type == "pointer" and last.buttons == buttons
             and wheel == 0 and last.wheel == 0 then
            last.x, last.y = x, y
          else
            v.input[#v.input + 1] = { type = "pointer", x = x, y = y,
                                      buttons = buttons, wheel = wheel }
          end
        end

        v.mask = mask
      elseif kind == 6 then
        -- ClientCutText: the length, then the text, which is not used yet.
        local n = string.unpack(">I4", inb, 5)

        if n > 1048576 then return false, "a clipboard larger than a megabyte" end
        if #inb < 8 + n then return true end

        v.inb = inb:sub(9 + n)
      end
    end
  end
end

--------------------------------------------------------------------------
-- The loop.
--------------------------------------------------------------------------

publish()

-- About thirty looks a second while somebody is waiting for a change; a
-- quarter of a second otherwise.
local EAGER = math.max(1, TICK_HZ // 30)
local IDLE = math.max(1, TICK_HZ // 4)

while true do
  local reading, writing, eager = {}, {}, false

  for _, v in ipairs(viewers) do
    reading[#reading + 1] = v.conn

    if #v.out > 0 or v.job then writing[#writing + 1] = v.conn end
    if v.want then eager = true end
  end

  local ready, arrived = fs.poll("/Network", reading, writing, net.listener,
                                 eager and EAGER or IDLE)

  if not ready then
    print("vncd: poll: " .. tostring(arrived))
    break
  end

  -- A viewer, from this network; anybody else was turned away.
  if arrived then
    local conn, from = net:accept(1)

    if conn then open(conn, from) end
  end

  -- What changed on the screen, to everybody who has seen it. **This is
  -- also what keeps a stale copy from being sent**: a copy the window
  -- manager let go while nobody looked is found out here, on the pass a
  -- viewer finishes its handshake, and watched again - whole - before that
  -- viewer can ask for a frame, which it does only after reading ServerInit.
  local anybody = false

  for _, v in ipairs(viewers) do
    if v.stage == "normal" then anybody = true end
  end

  if anybody and screen then
    local list = screen.watching and wmproto.watched(screen)

    if not list then
      -- Let go while nobody asked; watched again, and the whole screen
      -- comes with it.
      if watch() then list = wmproto.watched(screen) end
    end

    for _, r in ipairs(list or {}) do
      for _, v in ipairs(viewers) do
        if v.stage == "normal" then add_pending(v, r[1], r[2], r[3], r[4]) end
      end
    end
  end

  for at = #viewers, 1, -1 do
    local v = viewers[at]
    local piece = v.conn:read()

    while piece do
      v.inb = v.inb .. piece
      piece = v.conn:read()
    end

    local ok, why_closed = take(v)

    -- What the viewer did, to the desktop, in the order it did it.
    for i = 1, #v.input do
      fs.send(wmproto.REMOTE, v.input[i])
      v.input[i] = nil
    end

    if ok then
      maybe_update(v)
      pump(v)
    end

    while #v.out > 0 do
      local head = v.out[1]
      local wrote = v.conn:write(head)

      if not wrote or wrote == 0 then break end

      if wrote < #head then
        v.out[1] = head:sub(wrote + 1)
      else
        table.remove(v.out, 1)
      end

      v.queued = v.queued - wrote
      pump(v)
    end

    if not ok then
      v.leaving = v.leaving or why_closed or "closed"
    end

    if v.conn:closed() or (v.leaving and #v.out == 0) then
      close(at, v.leaving)
    end
  end

  -- A request from the Servers window: end an address's viewers.
  net:disconnects(disconnect)
end

for at = #viewers, 1, -1 do close(at) end
