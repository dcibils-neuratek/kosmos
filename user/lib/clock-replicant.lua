-- A clock, as a replicant: source, state, and what it needs.
--
-- This file is never `use`d. It is *read* - as text - by whatever wants to
-- hand it to somebody else, and loaded by whoever receives it, into an
-- environment built from the `needs` list beside it and nothing more.
--
-- So it may ask /dev/cpu how fast the counter runs, and it may not read a
-- file, list a directory, draw outside its own rectangle, or find out that
-- any of those things exist.
--
-- It returns a factory, which is the contract: a function taking the state
-- table and returning something that can draw itself.

return function(state)
  local clock = { since = 0, label = state.label or "up" }

  local cpu = fs.read("/dev/cpu")
  clock.hz = cpu and cpu.counter_hz or 1

  --
  -- What this can actually reach, tried from in here.
  --
  -- The host cannot answer this on the replicant's behalf. It tried once:
  -- `tracker` built the same restricted namespace and probed that, which
  -- measured the function rather than the environment - open the sandbox in
  -- `ui.replicant` and the probe went on happily reporting a refusal.
  --
  -- So the question is asked from inside the thing being asked about. These
  -- two fields are the only honest evidence about what this environment is.
  --
  clock.declared = cpu ~= nil
  clock.escaped = fs.read("/data/replicants/clock") ~= nil

  function clock:tick()
    self.since = ticks() // self.hz
  end

  function clock:draw(g, w, h)
    g:fill(0, 0, w, h, theme.sunken)
    g:frame(0, 0, w, h, theme.line)

    local m = self.since // 60
    local s = self.since % 60
    local text = string.format("%s %02d:%02d", self.label, m, s)

    g:text((w - #text * gfx.font.w) // 2,
           (h - gfx.font.h) // 2, text, theme.good, theme.sunken)
  end

  clock:tick()
  return clock
end
