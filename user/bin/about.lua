-- kosmos: application
-- About Kosmos.
--
-- BeOS's About box: the machine down the left, and text down the right. In
-- BeOS the right-hand column was trademark notices, which is what a
-- commercial system has to put there. This one has room for something
-- better, so it says what the system is and why it is built the way it is.
--
-- Every number on the left is read from the same nodes every other program
-- here reads - /dev/cpu, /dev/kernel, /dev/memory - and the version comes
-- from `sys.build()`, which the Makefile compiles in from the commit.

local ui = use("/lib/ui.lua")
-- The *kit's* palette, not a copy of it.
--
-- `use` runs the chunk again and hands back a different table, and only the
-- one `ui.lua` holds is the one it mutates when the desktop changes theme.
-- An application that loaded its own kept the colours it started with while
-- every widget around it changed - which is exactly what Monitor, Processes,
-- Photo and the Terminal did.
local theme = ui.theme

local W, H = 620, 420

local win, err = ui.window{ title = "About Kosmos", w = W, h = H,
                            x = 120, y = 80 }

if not win then
  print("about: " .. tostring(err))
  return
end

local b = sys.build()
local cpu = fs.read("/dev/cpu") or {}
local mem = fs.read("/dev/memory") or {}
local hz = cpu.counter_hz or 62500000

--------------------------------------------------------------------------
-- The name, in the one place this system allows itself a flourish.
--------------------------------------------------------------------------

local banner = ui.view{ x = 16, y = 14, w = 230, h = 54 }

function banner:draw(g)
  g:fill(0, 0, self.w, self.h, "sunken")
  g:frame(0, 0, self.w, self.h, "line")

  -- Two words, two colours, the way the BeOS logo split Be and OS.
  g:text(14, 18, "Kosm", "text", "sunken")
  g:text(14 + 4 * gfx.font.w, 18, "OS", "tab", "sunken")
  g:text(14 + 7 * gfx.font.w, 22, "  " .. b.version, "text_dim",
         "sunken")
end

win:add(banner)

--------------------------------------------------------------------------
-- The machine, down the left.
--------------------------------------------------------------------------

local facts = {}
local y = 82

local function fact(label, value)
  win:add(ui.label{ x = 16, y = y, text = label, color = "text" })
  local v = ui.label{ x = 16, y = y + 16, text = value,
                      color = "text_dim" }
  win:add(v)
  facts[label] = v
  y = y + 42
end

fact("Platform:", b.platform)
fact("Processor:", ("%s, %d core%s"):format(cpu.part or "unknown",
                                            cpu.cores or 1,
                                            (cpu.cores == 1) and "" or "s"))
fact("Kernel:", ("%s, built %s"):format(b.build, b.date))
fact("Running:", "just started")
fact("Memory:", ("%d MB, %d free"):format(mem.total_mb or 0,
                                          mem.free_mb or 0))

--------------------------------------------------------------------------
-- What it is, down the right.
--------------------------------------------------------------------------

win:add(ui.label{ x = 270, y = H - 26,
                  text = "drag or use the arrows to read on",
                  color = "line" })

win:add(ui.text{
  x = 270, y = 14, w = W - 286, h = H - 48,
  blocks = {
    { style = "title", text = "What Kosmos is" },

    { style = "body", text =
      "A microkernel with a Lua userland, on AArch64. The kernel knows " ..
      "about threads, address spaces, IPC and capabilities, and about " ..
      "nothing else. There is no filesystem in it, no network, no " ..
      "graphics beyond the boot screen, and no allocator anywhere." },

    { style = "head", text = "Everything else is a process" },

    { style = "body", text =
      "The filesystem is a process. The console is a process. This " ..
      "window's pixels live in another process, which is why an " ..
      "application that stops answering still has a window you can drag. " ..
      "They talk by sending messages, and a message can only go somewhere " ..
      "the sender was handed." },

    { style = "head", text = "You cannot name what you were not given" },

    { style = "body", text =
      "A process holds capabilities by index. There is no global name for " ..
      "anything, so a program that was not handed /dev does not get " ..
      "permission denied - the path does not exist for it. Every window " ..
      "you see was started with exactly what it needs and nothing more." },

    { style = "head", text = "Lua unless it must be C" },

    { style = "body", text =
      "The test is whether a bug can escape. If it can corrupt another " ..
      "process it is C; if it can only kill its own, it is Lua. So the " ..
      "kernel, the interpreter and the pixel loops are C, and the " ..
      "filesystem, the shell, this desktop and every application are Lua " ..
      "- and can be replaced while the machine runs." },

    { style = "head", text = "BeOS's structure, not its skin" },

    { style = "body", text =
      "The tab is as wide as its title, so several stacked windows stay " ..
      "readable at once: a decision about behaviour, copied exactly. The " ..
      "grey bevels and the 1998 palette are decisions about shading, and " ..
      "those are made fresh." },

    { style = "accent", text =
      "A personal learning project. No users, no compatibility to keep, " ..
      "no deadline - which is what makes it possible to take decisions a " ..
      "commercial system cannot." },
  },
})

--------------------------------------------------------------------------
-- Uptime, which is the only thing on this window that changes.
--------------------------------------------------------------------------

local ticker = ui.view{ x = 0, y = 0, w = 0, h = 0 }

function ticker:tick()
  local up = sys.ticks() // hz
  local m = fs.read("/dev/memory") or {}

  facts["Running:"].text = ("%d minutes, %d seconds"):format(up // 60, up % 60)
  facts["Memory:"].text = ("%d MB, %d free"):format(m.total_mb or 0,
                                                    m.free_mb or 0)
end

win:add(ticker)
win:run()
