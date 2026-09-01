-- How many pixels a second this machine can actually move.
--
--   gfxbench
--
-- Asked because "the game is slow" is not a number, and the question
-- underneath it - can this system run Doom at 35 frames a second - is
-- arithmetic once you have one.
--
-- Measured with `sys.ticks`, the 62.5 MHz physical counter, and reported as
-- megabytes a second so it can be compared with what a frame costs. Under
-- QEMU this measures *QEMU*, not hardware: every store the guest makes is
-- translated. `testing.md` 18.3 says the same thing about the benchmarks -
-- these numbers detect a regression, they do not predict a Pi.

local hz = fs.read("/dev/cpu").counter_hz

local function timed(what, pixels, fn)
  local began = sys.ticks()
  fn()
  local took = sys.ticks() - began

  if took <= 0 then took = 1 end

  local per_second = pixels * hz // took
  local mb = per_second * 4 // (1024 * 1024)

  print(("  %-22s %8d px in %5d us   %4d Mpx/s  %4d MB/s")
        :format(what, pixels, took * 1000000 // hz,
                per_second // 1000000, mb))

  return per_second
end

local W, H = 608, 700                 -- the Pac-Man window
local a = gfx.surface { w = W, h = H }
local b = gfx.surface { w = W, h = H }

print(("a %dx%d surface is %d pixels, %d KB")
      :format(W, H, W * H, W * H * 4 // 1024))
print("")

local N = 20

timed("fill, whole surface", W * H * N, function()
  for _ = 1, N do a:fill(0, 0, W, H, 0xff102030) end
end)

local blit_rate = timed("blit, whole surface", W * H * N, function()
  for _ = 1, N do b:blit(a, 0, 0, W, H, 0, 0) end
end)

timed("span, one row at a time", W * H, function()
  for y = 0, H - 1 do a:span(0, y, W, 0xff203040) end
end)

-- A disc is what Pac-Man draws five of; a span per row.
timed("disc, radius 15, x1000", 1000 * 30 * 30, function()
  for _ = 1, 1000 do
    for dy = -15, 15 do
      local dx = math.floor(math.sqrt(225 - dy * dy))
      a:span(300 - dx, 350 + dy, dx * 2 + 1, 0xffffff00)
    end
  end
end)

timed("gfx.disc in C, x1000", 1000 * 30 * 30, function()
  for _ = 1, 1000 do a:disc(300, 350, 15, 0xffffff00) end
end)

print("")
print("what that means:")

local function budget(name, w, h, fps)
  local need = w * h * fps
  print(("  %-26s %d x %d at %d fps needs %d Mpx/s  -> %s")
        :format(name, w, h, fps, need // 1000000,
                (blit_rate > need * 2) and "comfortable"
                or (blit_rate > need) and "tight" or "too slow"))
end

budget("Doom",            320, 200, 35)
budget("this Pac-Man",    W,   H,   30)
budget("a 400x320 window", 400, 320, 30)

print("")
print("A frame is at least two passes: the application draws it and the")
print("compositor blits it. Halve the figures above for a real budget.")
