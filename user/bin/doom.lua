-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Doom.
-- kosmos: application
-- kosmos: section demos
--
--   wm doom                    /home/doom1.wad
--   wm doom:/data/other.wad    somewhere else
--
-- Only in an image built with `make DOOM=1`; see
-- `runtime/upstream/doom/README.md` for why that is a build option and not
-- part of the desktop.
--
-- **This file is the loop, and that is the whole division.** id's code does
-- the game; `doom_kosmos.c` gives it six functions; and what is left - which
-- window, which WAD, when a frame happens, when to stop - is policy, so it
-- is here in Lua where policy goes. A port that owned its own loop would be
-- a window you could not close.
--
-- The WAD does not pass through Lua. `USER_HEAP_PAGES` is 512, so the Lua
-- heap is two megabytes and the smallest IWAD is four: it *cannot* be a
-- string here. It goes into a region instead, which `fs.read_into` fills
-- straight from the disk server, and Doom reads it where it lies. The limit
-- was pointing at the better design.

local ui = use("/lib/ui.lua")

if type(doom) ~= "table" then
  print("doom: this image was not built with DOOM=1")
  return
end

local path = (args or ""):match("^%s*(%S+)") or "/home/doom1.wad"

local attrs, why = fs.getattr(path)

if not attrs then
  print("doom: no " .. path .. ": " .. tostring(why))
  print("      put one on the disk: make image FILES=\"doom1.wad:/home/doom1.wad\"")
  return
end

local size = attrs.size or 0

if size < 12 then
  print("doom: " .. path .. " is " .. size .. " bytes, which is not a WAD")
  return
end

--
-- The WAD, in a region of its own.
--
-- One page more than it needs, because a partial last page is still a whole
-- page and asking for the exact byte count would round down.
--
local pages = (size + 4095) // 4096
local wad = sys.memory(pages)

if not wad then
  print(("doom: no room for %d KB of WAD"):format(size // 1024))
  return
end

local at = sys.memory_map(wad)

if not at then
  print("doom: the WAD region would not map")
  return
end

--
-- Into the region, a window at a time, through a scratch region.
--
-- `fs.read_into(path, region, offset, bytes)` takes a *file* offset and
-- always writes at the start of the region: there is no region offset in
-- the protocol. A loop that ignored that - which is what this was first -
-- writes every window over the last one, and what ends up at the front of
-- the WAD is its final megabyte. The check below caught it saying "02_8"
-- where "IWAD" belongs.
--
-- So each window lands in a scratch region, comes out as a string, and goes
-- into the big one at the right offset. That is one extra copy per window
-- and it is the honest cost of the protocol as it stands. The alternative
-- is an `into_offset` in the read request, which is a change to the
-- filesystem protocol and to every server that implements it - worth doing,
-- and not worth doing in the middle of getting Doom to boot.
--
do
  local WINDOW = 256 * 1024
  local scratch = sys.memory(WINDOW // 4096)

  if not scratch then
    print("doom: no room for a staging window")
    return
  end

  local done = 0

  while done < size do
    local want = math.min(WINDOW, size - done)
    local got = fs.read_into(path, scratch, done, want)

    if not got or got == 0 then
      print(("doom: %s stopped after %d of %d bytes"):format(path, done, size))
      return
    end

    sys.region_write(wad, done, sys.region_read(scratch, 0, got))
    done = done + got
  end
end

--
-- Is it actually a WAD?
--
-- Checked here rather than left to Doom, because Doom's answer - "Wad file
-- doom1.wad doesn't have IWAD or PWAD id" - is about the *file*, and the
-- thing that goes wrong on this system is the *transfer*. Reading the first
-- four bytes back out of the region says which: if they are IWAD then the
-- bytes arrived and anything after this is Doom's business.
--
do
  local magic = sys.region_read(wad, 0, 4)

  if magic ~= "IWAD" and magic ~= "PWAD" then
    print(("doom: %s starts %q, not IWAD - the read did not land")
          :format(path, tostring(magic)))
    return
  end
end

local W, H = doom.width, doom.height

--
-- `direct` because a frame is 640x400 pixels thirty-five times a second.
--
-- Sending that as drawing commands is not a thing to consider: it is a
-- megabyte a frame through a 2048-byte message. A direct window is a
-- surface this process writes into and the compositor blits, which is the
-- bargain `gfx.md` 19.4 describes and exactly what a game wants.
--
local win, err = ui.window{ title = "Doom", w = W, h = H, x = 60, y = 60,
                            direct = true }

if not win then
  print("doom: " .. tostring(err))
  return
end

if not win:surface() then
  print("doom: this window did not get a shared surface")
  return
end

print(("doom: %s, %d KB"):format(path, size // 1024))

--
-- Anything Doom printed, onto this program's console.
--
-- `printf` in a process that does not own the console is refused by the
-- kernel and lands in a ring instead - see `stdio.c`. This is the only
-- thing that empties it, so it is called around the parts that talk and
-- once a frame after.
--
local function drained()
  local said = doom.log()

  while said do
    print(said)
    said = doom.log()
  end
end

local ok, started, why = pcall(doom.start, at, size)

-- Before the verdict, because whatever Doom said about it is in the ring
-- and is the part worth reading.
drained()

if not ok then
  print("doom: " .. tostring(started))
  return
end

if not started then
  print("doom: " .. tostring(why or "did not start"))
  return
end

--
-- Keys, as transitions.
--
-- The window manager sends two streams. `key` is characters - what a key
-- *meant*, shifted and mapped, with an arrow spread over three bytes - and
-- is what a terminal wants. `rawkey` is what the key *did*: a keycode and
-- whether it went down.
--
-- A game wants the second, and not as a preference. Holding a direction is
-- a question about the key itself, and no stream of characters can express
-- it: a character is a press with no release behind it, so walking forward
-- came out as a step per key-repeat. Reading transitions, Doom is told the
-- key went down and hears nothing more until it comes up, which is exactly
-- what it wants.
--
-- It also disposes of the escape-sequence problem rather than solving it.
-- The left arrow is keycode 105 whatever else is going on; it is only as a
-- *character* that it needs three bytes and a decoder.
--
-- The codes are Linux's `input-event-codes.h`, which is what virtio-input
-- speaks and what the HAL passes up undecoded. The Doom side is
-- `doomkeys.h`. This table is the whole of the translation.
--
local KEYS = {
  [103] = 0xad,       -- up      -> KEY_UPARROW
  [108] = 0xaf,       -- down    -> KEY_DOWNARROW
  [105] = 0xac,       -- left    -> KEY_LEFTARROW
  [106] = 0xae,       -- right   -> KEY_RIGHTARROW

  [1]   = 27,         -- escape  -> KEY_ESCAPE, the menu
  [28]  = 13,         -- enter   -> KEY_ENTER
  [15]  = 9,          -- tab     -> KEY_TAB, the automap
  [14]  = 127,        -- backspace

  [29]  = 0xa3,       -- left ctrl  -> KEY_FIRE
  [97]  = 0xa3,       -- right ctrl
  [57]  = 0xa2,       -- space      -> KEY_USE
  [42]  = 0xb6,       -- left shift -> KEY_RSHIFT, which is run
  [54]  = 0xb6,       -- right shift
  [56]  = 0xb8,       -- left alt   -> KEY_LALT, which is strafe

  -- The number row picks a weapon; y and n answer the prompts.
  [2] = 49, [3] = 50, [4] = 51, [5] = 52,
  [6] = 53, [7] = 54, [8] = 55,

  [21] = 121,         -- y
  [49] = 110,         -- n
}

--
-- The one key here that is not Doom's: Control-C closes the window.
--
-- By keycode, so it does not depend on the character path at all - left
-- control is 29 and `c` is 46.
--
local CTRL = { [29] = true, [97] = true }

local ctrl_down = false

--
-- The loop, which is cube3d's and plasma's shape rather than `win:run()`.
--
-- A direct window is not driven by widgets: there is nothing for a view to
-- paint, because the pixels arrive in the shared surface. So this is the
-- same three steps every direct application here does - render, commit the
-- damage, poll for events - and the only thing Doom adds is that the render
-- step is somebody else's forty thousand lines.
--
while win.running do
  --
  -- Asked for on every pass, not captured once.
  --
  -- A direct window is double-buffered, and `commit` flips which of the two
  -- is live - so a surface fetched before the loop is the right one for
  -- exactly one frame and the wrong one for ever after. What that looks
  -- like is a game that runs at sixty frames a second onto a window that
  -- stays black, which is what this did. cube3d asks inside its loop for
  -- the same reason.
  --
  local fine, oops = pcall(doom.frame, win:surface())

  if not fine then
    print("doom: " .. tostring(oops))
    break
  end

  drained()

  if not win:commit{ x = 0, y = 0, w = W, h = H } then
    break
  end

  --
  -- `wait = 0`: do not block.
  --
  -- cube3d asks for a tick of waiting so an idle desktop is idle, and that
  -- is right for a spinning cube, which has nowhere to be. Doom paces
  -- itself - `DG_SleepMs` holds it to thirty-five tics a second and yields
  -- while it waits - so waiting here as well would halve the frame rate
  -- for nothing.
  --
  local reply = fs.send("/app/wm", { type = "poll", window = win.handle,
                                     wait = 0 })

  if not reply then break end

  for _, ev in ipairs(reply.events or {}) do
    if ev.type == "close" then
      win:close()
    elseif ev.type == "rawkey" then
      if CTRL[ev.code] then ctrl_down = ev.down end

      -- Control-C, like every other application here.
      if ctrl_down and ev.code == 46 and ev.down then
        win:close()
      else
        local k = KEYS[ev.code]

        -- Down and up, each as it happens. This is the whole fix for
        -- holding a direction: Doom is told the key went down and hears
        -- nothing more until it comes up.
        if k then doom.key(k, ev.down) end
      end
    end
  end
end

win:close()
