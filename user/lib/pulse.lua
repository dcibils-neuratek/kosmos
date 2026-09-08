-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- The processor panel, drawn the way BeOS drew one.
--
-- An identity box saying what processor this is, and one segmented bar per
-- processor beside it, each with its number in a chip. That is Pulse's
-- layout and it is taken deliberately rather than fondly: **it was designed
-- for a number nobody knows in advance.** Two on the Pentium II it shipped
-- for, four here, twenty on the laptop in `docs/targets.md` - the shape is a
-- stack of rows, so a machine with more processors gets a taller window and
-- never a scrollbar.
--
-- Here because two programs draw it. `sysmon` is the one you leave open and
-- `cores` is the one with the buttons that make something happen; the panel
-- between them is the same panel, and a second copy of it would be a second
-- copy to keep right.
--
-- **What this does not do is read anything.** It is handed a count, a
-- scheduling count and a function that answers a percentage, because a
-- widget that called `sys.info` itself would be a widget with an opinion
-- about which machine it is on. `ui.leds` underneath it is the same
-- bargain one layer down: it draws segments and knows nothing about
-- processors.
--
local ui    = use("/lib/ui.lua")
local theme = ui.theme

local pulse = {}

local ROW     = 22                  -- one processor
local CHIP_W  = 16
local IDENT_W = 132

--
-- How tall a panel with this many processors wants to be.
--
-- The taller of its two halves, which is what makes the reference's
-- proportions hold at any count: with two processors the identity box sets
-- the height and from four upwards the bars do.
--
function pulse.height(cores, ident_lines)
  local bars  = cores * ROW + 8
  local ident = (ident_lines or 4) * (gfx.font.h + 3) + 8

  return (bars > ident) and bars or ident
end

--
-- What processor this is, in the words `/dev/cpu` uses.
--
-- **The rate is labelled "counter" and that is not pedantry.** Pulse said
-- "450 MHz" and meant the core clock; `/dev/cpu` has no such number on
-- either of this system's boards. What it has is the frequency of the
-- counter the kernel measures time with - 62.5 MHz under TCG, 24 under hvf,
-- a thousand on the other machine - and printing that where a person reads
-- a clock speed would be the same mistake this system has already made with
-- these units twice, one board apart. So it says which clock it is.
-- `architecture.md` §5 is why there are two.
--
function pulse.identity()
  local cpu  = fs.read("/dev/cpu") or {}
  local out  = {}

  if cpu.implementer then out[#out + 1] = cpu.implementer end
  if cpu.part        then out[#out + 1] = cpu.part end
  if cpu.revision    then out[#out + 1] = cpu.revision end

  if cpu.counter_hz and cpu.counter_hz > 0 then
    out[#out + 1] = ("counter %d MHz"):format(cpu.counter_hz // 1000000)
  end

  if #out == 0 then out[1] = cpu.arch or "processor" end

  return out
end

--
-- The panel itself.
--
--   cores       how many rows - every processor the machine has
--   scheduling  how many of them run threads; the rest are drawn parked
--   read(i)     percentage for processor i, 1-based, or nil
--   ident       the lines for the identity box
--
function pulse.panel(spec)
  local v = ui.view{ x = spec.x, y = spec.y, w = spec.w,
                     h = pulse.height(spec.cores, #spec.ident) }

  v.cores      = spec.cores
  v.online     = spec.online or spec.scheduling
  v.scheduling = spec.scheduling
  v.read       = spec.read
  v.ident      = spec.ident

  function v:draw(g)
    --
    -- The recess everything sits in, which is the frame Pulse drew and the
    -- reason its panel reads as a piece of equipment rather than as paint.
    --
    g:fill(0, 0, self.w, self.h, "raised")
    g:frame(0, 0, self.w, self.h, "edge_dark")
    g:frame(1, 1, self.w - 2, self.h - 2, "edge_light")

    local ix, iy, iw = 4, 4, IDENT_W - 8

    g:fill(ix, iy, iw, self.h - 8, "sunken")
    g:frame(ix, iy, iw, self.h - 8, "line")

    for i = 1, #self.ident do
      -- The second line is the part number, which is the one a person is
      -- looking for; the rest are context.
      g:text(ix + 6, iy + 5 + (i - 1) * (gfx.font.h + 3), self.ident[i],
             (i == 2) and "text" or "text_dim")
    end

    local bx = IDENT_W + 6

    for c = 1, self.cores do
      local ry    = 4 + (c - 1) * ROW
      -- Online, not scheduling. A processor that takes its own timer
      -- interrupt and charges its own idle time has a real reading to show,
      -- whether or not anything is ever scheduled onto it - and `parked` for
      -- one of those would be the same collapse of two facts into one number
      -- that `kernel/syscall.h` records.
      local live  = (c <= self.online)
      local value = live and (self.read(c) or 0) or 0
      local lit   = (value > 80) and theme.bad or theme.good

      --
      -- The chip. Lit when this processor has reached the kernel, dark
      -- when it never started.
      --
      -- **The comment here used to say "lit when this processor is
      -- scheduling", and the line below has always said `c <= self.online`,
      -- which is a different question.** Every core that reaches the kernel
      -- arms its own timer and charges its own ticks, so it has a real
      -- reading whether or not anything was placed on it. Whether it is
      -- *given work* is placement, and that is what the note at the bottom
      -- of `cores` says in words.
      --
      -- Pulse's numbers were buttons that took a processor offline. This
      -- kernel cannot stop a core, so they are indicators - and they carry
      -- the one thing this machine has to say that Pulse's did not. **A
      -- processor that never started gets a row and not a number**: its
      -- `idle_ticks` and `busy_ticks` were never written by anything, and a
      -- bar at 0% would claim it was measured and found idle. Those are
      -- different facts.
      --
      local chip = live and theme.good or theme.lift(theme.good, -100)

      g:fill(bx, ry, CHIP_W, ROW - 4, chip)
      g:frame(bx, ry, CHIP_W, ROW - 4, "edge_dark")
      g:text(bx + (CHIP_W - gfx.font.w) // 2, ry + 1, tostring(c - 1),
             live and "text_on" or "text_dim")

      --
      -- The reading, right of the bar and on the same line as it.
      --
      -- Pulse had no numbers at all - the bars were the whole display. This
      -- keeps one, because the meter it replaces had one and losing it
      -- would be taking something away in exchange for a look. Six
      -- characters reserved, which is `parked` and is the longest thing
      -- this can say.
      --
      -- "parked" was the word while a secondary started and sat in `wfi`.
      -- One that is not `live` never reached the kernel at all, which is
      -- the stronger statement and the one this branch means.
      local text = live and ("%d%%"):format(value) or "no data"
      local room = 6 * gfx.font.w + 6

      local barx = bx + CHIP_W + 6
      local barw = self.w - 4 - barx - room

      g:frame(barx, ry, barw, ROW - 4, "edge_dark")
      ui.leds(g, barx + 1, ry + 1, barw - 2, ROW - 6, value / 100, lit)

      g:text(self.w - 4 - #text * gfx.font.w, ry + 1, text,
             live and "text" or "text_dim")
    end
  end

  return v
end

return pulse
