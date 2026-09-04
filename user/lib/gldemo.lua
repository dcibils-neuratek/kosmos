-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- One TinyGL demo, in a window.
--
-- The body every `gl*` application shares. Each of them is four lines - a
-- name and a title - because the difference between them is which C
-- function runs, and that is TinyGL's business rather than this file's.
--
-- **The demos are C and were not rewritten.** They are TinyGL's own,
-- vendored unmodified in `runtime/upstream/tinygl/examples/`, renamed apart
-- on the compile line so eight programs that each define a function called
-- `draw` can share one binary. Doom is arranged this way for the same
-- reason: transcribing them into Lua would be eight chances to get a normal
-- backwards, and the next upstream release could no longer be dropped in.
--
-- **A direct window**, as `cube3d` and `doom` are: this draws its own
-- pixels into a surface shared with the compositor rather than recording a
-- display list, because there is nothing to record - a rasteriser hands
-- over a finished picture. `gfx.md` 19.4.
--
-- **The size is not arbitrary.** A GL context costs ten bytes a pixel - a
-- colour buffer and a conversion buffer at four each, a depth buffer at two
-- - out of a two-megabyte heap that Lua is already living in. 360x330 wants
-- about 1.2 MB and leaves room; 460x380 wants 1.7 MB and used to kill the
-- process with nothing said, because TinyGL asserts rather than returning
-- when an allocation fails and an assert here is a panic. The kit refuses
-- an impossible size now and says what would fit.

local ui = use("/lib/ui.lua")
local gl = use("/kits/gl")

return function(name, title)
  local BG, DIM = 0xff101828, 0xff7c8ba0

  local win, err = ui.window{ title = title or name, w = 360, h = 330,
                              x = 190, y = 120, direct = true }

  if not win then
    print(name .. ": " .. tostring(err))
    return
  end

  if not win:surface() then
    print(name .. ": this window did not get a shared surface")
    return
  end

  -- The surface, not what was asked for: the compositor keeps a title bar
  -- and a context built for rows that do not exist is a picture drawn for a
  -- rectangle nobody has.
  local W, H = win:surface():size()

  local ctx, why = gl.context(W, H)

  if not ctx then
    print(name .. ": " .. tostring(why))
    return
  end

  local ok, oops = gl.start(name, W, H)

  if not ok then
    print(name .. ": " .. tostring(oops))
    return
  end

  local frames, from, fps = 0, sys.ticks(), 0
  local hz = (fs.read("/dev/cpu") or {}).counter_hz or 62500000

  while win.running do
    local s = win:surface()

    --
    -- One frame from the demo, then the copy.
    --
    -- `frame` is `idle` and `draw` back to back, the order `ui_loop` uses.
    -- The blit is separate because TinyGL renders into a buffer of its own
    -- and a surface has a pitch of its own - `gfx.md` 19.3, the discipline
    -- everything here obeys.
    --
    gl.frame()
    gl.blit(ctx, s, 0, 0)

    s:text(8, H - 18, ("%d fps   software rasterised"):format(fps), DIM)

    if not win:commit{ x = 0, y = 0, w = W, h = H } then break end

    frames = frames + 1

    local span = sys.ticks() - from

    if span >= hz then
      fps = (frames * hz) // span
      frames, from = 0, sys.ticks()
    end

    -- A tick of waiting rather than none: a loop that never blocks is a
    -- thread that is always runnable, and an idle desktop should be idle.
    local reply = fs.send("/dev/wm", { type = "poll", window = win.handle,
                                       wait = 1 })

    if not reply then break end

    for _, ev in ipairs(reply.events or {}) do
      if ev.type == "close" then
        win:close()
      elseif ev.type == "key" then
        if ev.code == 3 then
          win:close()
        else
          -- Anything else belongs to the demo. Several respond to arrows
          -- and letters - `mech` walks, `morph3d` changes solid - and
          -- passing the key through keeps this file from having opinions
          -- about eight programs it did not write.
          gl.key(ev.code)
        end
      end
    end
  end

  ctx:close()
end
