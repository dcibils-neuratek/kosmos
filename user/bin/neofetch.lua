-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The machine, in the space a login banner has.
--
--   neofetch
--
-- What Linux's neofetch does: a mark on the left, the machine in a column
-- beside it, short enough to read without scrolling and complete enough
-- that you do not have to ask anything else. It runs when the shell starts
-- and when a Terminal opens, which is the only two moments anybody wants
-- it, and `machine` is still there for the long answer.
--
-- **Three of neofetch's fields are missing, and each of them is missing
-- for a reason worth knowing.**
--
-- There is no `user@host` line, because there is no user and no host name.
-- Nothing in this system has a global name - that is the first principle,
-- not an omission - so there is nothing to put on either side of the `@`.
-- The header is the system and its version, which is what the question
-- "what am I typing at" actually has an answer to.
--
-- There is no `Terminal:` line, because a program **cannot** find out. It
-- prints by sending `write` to whatever it was handed as `/dev/console`,
-- and a Terminal window mounts itself there speaking the same protocol the
-- console server does. The runner cannot tell the two apart and must not
-- need to, which is `terminal.lua`'s whole design. A field that guessed
-- would be inventing an answer to a question the namespace exists to make
-- unanswerable.
--
-- And there are no colour bars along the bottom, because `conproto.h`
-- carries text and nothing else - a console `write` is bytes, with no
-- attribute beside them. That row is the kernel's fixed pools instead,
-- which is a better use of it than sixteen squares: with no allocator
-- anywhere in the kernel, every one of those is an array declared at
-- compile time, and how full they are is the one number this system wants
-- somebody to have seen. Every resource bug it has had was a pool filling
-- up - capability slots gone on the sixteenth read, a region per font per
-- size, the process table full at round twenty-two - and each of them was
-- found by looking at exactly this line after using the machine.

--------------------------------------------------------------------------
-- The banner.
--
-- `assets/kosmos-ascii-art.txt`, carried in the image and asked for by name.
-- `sys.asset` is "a small file compiled in", which is exactly what this is,
-- and it is already how the test pattern arrives - so the banner needed no
-- new mechanism, only a line in the Makefile.
--
-- **Two colours, and neither is written in the file.** The art is plain
-- UTF-8 with no markup, and it does not need any: the diagonals are drawn
-- with Block Elements and the wordmark with ASCII line art, so which is
-- which is a property of the glyphs. That is worth preferring to a marked-up
-- file - the picture stays something you can open in an editor and change,
-- and there is no second syntax to keep in step with it.
--
-- The three shades take care of themselves. `+` and `-` are one colour and
-- the light shade simply covers fewer pixels, which is why the faint
-- diagonals in the picture are faint without anything here knowing about
-- them.
--------------------------------------------------------------------------

-- Block Elements are U+2580 to U+259F, which in UTF-8 is 0xE2 0x96 0x80 to
-- 0xE2 0x96 0x9F - a three-byte pattern with a fixed first two, so a Lua
-- pattern over bytes finds them without decoding anything.
local BLOCK = "\226\150[\128-\159]"

-- Literal colours rather than names, and deliberately: these are a logo, not
-- a status. `good` and `bad` mean something and should follow whatever the
-- console's palette says they are; this blue and this red mean Kosmos.
local STRIPE = 0xff2233ee
local MARK   = 0xffcc2233

--
-- One line of the banner, as runs.
--
-- A run ends where the glyph class changes, so a row of the wordmark - which
-- has stripes either side of it and through it - goes out as five or six
-- writes and arrives as one line. That is the console protocol working as
-- intended: a colour belongs to the bytes it applies to, and a line in
-- several colours is several writes rather than a list of runs on the wire.
--
local function banner_line(line)
  local at = 1

  while at <= #line do
    local from, to = line:find(BLOCK, at)

    if not from then
      write(line:sub(at), MARK)
      return
    end

    if from > at then write(line:sub(at, from - 1), MARK) end

    -- As far as the blocks go, so a diagonal is one write and not one per
    -- character.
    while true do
      local _, more = line:find("^" .. BLOCK, to + 1)

      if not more then break end
      to = more
    end

    write(line:sub(from, to), STRIPE)
    at = to + 1
  end
end

--------------------------------------------------------------------------
-- What the machine says about itself.
--
-- Every one of these is read, and the ones that can be absent are allowed
-- to be: a board with no screen, no disk or no card is a real machine and
-- this has to describe it rather than print an empty value. `machine`
-- makes the same argument at length - present and absent are different
-- sentences - and the short form of it here is that a missing line reads
-- as "there is no such thing", so the line stays and says so.
--------------------------------------------------------------------------

local b      = sys.build()
local cpu    = fs.read("/dev/cpu")    or {}
local mem    = fs.read("/dev/memory") or {}
local kern   = fs.read("/dev/kernel") or {}
local screen = fs.read("/dev/screen")
local info   = sys.info() or {}

local LABEL = 10        -- columns the field names are given

local rows = {}

local function row(label, value)
  rows[#rows + 1] = ("%-" .. LABEL .. "s %s"):format(label, tostring(value))
end

--------------------------------------------------------------------------

--
-- The platform string carries the architecture already - it is "QEMU virt
-- aarch64" and "QEMU q35 x86-64" - so this does not append `cpu.arch` to
-- it. Two versions of that were written before anybody read the Makefile,
-- and both said "QEMU virt aarch64, aarch64".
--
row("Host", b.platform or "unknown machine")

-- Two names, and they are not interchangeable: Kosmos is the system this
-- banner is the banner of, Nebula is the microkernel under it. The version
-- is the header's, because they are built together and there is no second
-- number to give - so this line carries the commit instead, which is the
-- part the header does not have.
row("Kernel", ("%s, build %s"):format(b.kernel or "Nebula", b.build or "?"))

--
-- Uptime, on the counter - and the frequency is read three lines above the
-- division rather than remembered.
--
-- That is the discipline the two-clocks rule asks for, and it is cheap
-- here because `/dev/cpu` has already been read for the processor's name.
-- The number the counter runs at differs by a factor of four between this
-- machine emulated and the same machine run natively, so a constant would
-- be wrong on one of them and there is no ratio to keep in your head.
--
local hz = cpu.counter_hz

if hz and hz > 0 then
  local seconds = sys.ticks() // hz
  local minutes = seconds // 60
  local hours   = minutes // 60

  row("Uptime", (hours > 0)
      and ("%d hour%s, %d minute%s"):format(hours, hours == 1 and "" or "s",
                                            minutes % 60,
                                            minutes % 60 == 1 and "" or "s")
      or (minutes > 0)
      and ("%d minute%s"):format(minutes, minutes == 1 and "" or "s")
      or ("%d second%s"):format(seconds, seconds == 1 and "" or "s"))
else
  row("Uptime", "unknown; the counter did not answer")
end

--
-- The processor, named by whichever decoder in the devices server matched
-- the architecture - ARM's out of MIDR_EL1, x86's out of CPUID. Both fill
-- the same three field names precisely so that this does not have to know
-- which machine it is on.
--
local model = cpu.implementer
  and (("%s %s %s"):format(cpu.implementer, cpu.part or "",
                           cpu.revision or ""):gsub("%s+", " "):gsub(" $", ""))
  or (cpu.arch or "unknown")

--
-- Both counts when they differ, which on this machine they do unless it
-- was built with SMPWORK.
--
-- `cores_present` is what the firmware says the machine has and `cores` is
-- what this kernel schedules on. A banner that gave only the second would
-- call a four-processor machine a one-core computer - true about Kosmos,
-- false about the machine, and `About Kosmos` said exactly that until it
-- was fixed there for the same reason.
--
local present = cpu.cores_present or cpu.cores or 1
local using   = cpu.cores or 1

row("CPU", (present == using)
    and ("%s, %d core%s"):format(model, using, using == 1 and "" or "s")
    or ("%s, %d cores, %d scheduling"):format(model, present, using))

local total, free = mem.total_mb or 0, mem.free_mb or 0

row("Memory", ("%d of %d MB used"):format(total - free, total))

if screen then
  row("Display", ("%d x %d, 32-bit"):format(screen.width or 0,
                                            screen.height or 0))
else
  row("Display", "none; this machine is serial-only")
end

--
-- The disk, asked about through the filesystem rather than through the
-- block device. `/home/.super` is what the one process holding the disk
-- answers, and it is reachable by anything the shell started - a program
-- that went to the sectors would need a capability it has no business
-- holding, and `diskinfo` reads the same node for the same reason.
--
local sb = fs.read("/home/.super")

if sb and sb.present and sb.formatted then
  local block = sb.block_size or 4096
  local mb    = 1024 * 1024

  row("Disk", ("kfs, %d of %d MB free"):format(
        ((sb.free_blocks or 0) * block) // mb,
        ((sb.blocks or 0) * block) // mb))
elseif sb and sb.present then
  row("Disk", "attached, no filesystem (" .. tostring(sb.why) .. ")")
else
  row("Disk", "none; /home is in memory and will not survive")
end

--
-- **Two questions, and this program may only ask one of them.**
--
-- `net_info` answers through the stack, and a program that was not handed
-- `/net` gets nothing back - which is not the same fact as there being no
-- card, and reporting it as one is the exact lie `machine` was written to
-- avoid. So whether a card *exists* comes from `sysinfo`, which is the
-- kernel describing the machine rather than handing anything over, and the
-- address is shown only when this program actually holds the stack.
--
local net = fs.net_info("/net")

if net and type(net.address) == "string" and #net.address == 4 then
  row("Network", ("virtio-net at %d.%d.%d.%d"):format(net.address:byte(1, 4)))
elseif (info.net_mtu or 0) > 0 then
  row("Network", "virtio-net, no address configured")
else
  row("Network", "no card; this machine is on its own")
end

--
-- What there is to type, counted with one call and not with eighty-five.
--
-- `startup` tells an application from a program by asking `getattr` about
-- each file, and it is right to: it is a window somebody opened, once. This
-- runs every time a Terminal opens, and a round trip per file to split one
-- number into two would be eighty-five of them to save a reader four words.
-- Responsiveness is a design goal rather than a later optimisation, and a
-- banner is exactly the kind of thing that quietly stops obeying it.
--
row("Programs", ("%d in /bin"):format(#(fs.list("/bin") or {})))

--------------------------------------------------------------------------
-- The mark and the column, side by side.
--
-- Whichever runs out first is padded, which is what lets the two be edited
-- independently: adding a field does not mean redrawing the art.
--------------------------------------------------------------------------

--------------------------------------------------------------------------
-- The banner, then the machine.
--
-- Stacked rather than side by side, because the art is fifty columns wide
-- and a Terminal is seventy-eight: there is no room beside it for a field
-- and a value, and squeezing them in would mean an art file that could
-- never be edited without measuring what was next to it.
--------------------------------------------------------------------------

print("")

local art = sys.asset("kosmos-ascii-art.txt")

--
-- No banner is not an error. The facts are the part somebody actually asked
-- for, and a program that refused to report the machine because a picture
-- was missing would have its priorities the wrong way round.
--
if art then
  for line in tostring(art):gmatch("([^\n]*)\n?") do
    if line ~= "" then
      banner_line(line)
      write("\n")
    end
  end

  print("")
end

local header = ("%s %s"):format(b.name or "Kosmos", b.version or "?")

write(header .. "\n", "tab")
write(("-"):rep(#header) .. "\n", "tab")

--
-- Two writes a line: the field name and the value.
--
-- `write` is one run and adds no newline, which is the shape this wants -
-- the console joins the pieces because it appends until a newline arrives,
-- so there is no need for the protocol to carry a list of runs. The note on
-- `colour` in `conproto.h` is the long version.
--
for _, text in ipairs(rows) do
  write(text:sub(1, LABEL), "dim")
  write(text:sub(LABEL + 1))
  write("\n")
end

--------------------------------------------------------------------------
-- Where neofetch puts its colours.
--
-- Fixed pools, because the kernel has no allocator: every one of these is
-- an array declared at compile time, so running out is an error at a known
-- limit rather than a failure at an unknown one. Seeing them here is how
-- you notice a program that did not give something back - which is what
-- `make stress` exists to provoke and what this line exists to show.
--
-- `regions` comes from `sysinfo` rather than from `/dev/kernel`, which
-- does not carry it. It is the one most worth watching: a region is what
-- gets leaked per font, per size, per window, and it is the pool that has
-- filled up in practice.
--------------------------------------------------------------------------

print("")
write("  pools   ", "dim")
print((("%-16s%-16s%-16s%s"):format(
        ("threads %d/%d"):format(kern.threads or 0, kern.threads_max or 0),
        ("processes %d/%d"):format(kern.processes or 0,
                                   kern.processes_max or 0),
        ("endpoints %d/%d"):format(kern.endpoints or 0,
                                   kern.endpoints_max or 0),
        ("regions %d/%d"):format(info.regions_used or 0,
                                 info.regions_total or 0)):gsub("%s+$", "")))
print("")
