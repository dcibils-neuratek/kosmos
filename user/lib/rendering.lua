-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The rendering modes: how the desktop is drawn (`docs/rendering.md`).
--
-- One table for everything that names them - Preferences' Displays page,
-- the desktop's label, GPU Monitor - so a mode is said the same way
-- wherever it is said. Each is a step measured on the M700 (`m700-2d.md`,
-- `testing.md` 18.657-18.669) and each adds to the one before. The number
-- is what the window manager keeps under `rendering` in the appearance
-- file and what it asks the window server for (`WS_OP_RENDERING`).
--
-- `stages` is who does each of a frame's six stages, as `docs/composing.png`
-- draws them: "cpu", "gpu", "display", "none", or "gpu+cpu" for a stage
-- shared. "display" is the GPU's display engine - the part of the graphics
-- chip that reads a buffer and sends it to the monitor - and "gpu" its
-- drawing engines, the blitter and the 3D engine; `needs` says it in words
-- (Diego, 18 October: "what is 'the display'? the gpu itself? ... its
-- confusing", "framebuffer works without a gpu driver", "but page flips
-- does need gpu access").

local rendering = {}

rendering.STAGES = { "Draw", "Describe", "Compose", "Copy", "Show", "Pointer" }

-- Page flips: Diego, on the M700, trying each on 18 October - "page flips
-- is by far the smoothest". Blitter copy saves the processor its copy, but
-- the window server sees the blitter's fence a scheduler tick late and the
-- frame reaches the screen late by a varying amount; until the GPU's
-- interrupt wakes it, the eye prefers the processor's copy.
rendering.DEFAULT = 2

rendering.MODES = {
  [0] = {
    name = "Framebuffer", needs = "no GPU driver - the firmware's screen",
    sentence = "The processor composes each frame and copies it into the screen while "
               .. "the display reads it; the pointer is drawn into every frame. No GPU.",
    stages = { "cpu", "cpu", "cpu", "cpu", "cpu", "cpu" },
    memory = "none", note = "what Kosmos did before",
  },
  [1] = {
    name = "Cursor plane", needs = "the GPU's display engine",
    sentence = "As Framebuffer, and the display draws the pointer over the screen - "
               .. "moving the mouse composes nothing.",
    stages = { "cpu", "cpu", "cpu", "cpu", "cpu", "display" },
    memory = "16 KB", note = "a 64 x 64 picture",
  },
  [2] = {
    name = "Page flips", needs = "the GPU's display engine",
    sentence = "Two screens: the processor copies into the hidden one and the display "
               .. "flips to it at its blank - no tearing, a frame a refresh.",
    stages = { "cpu", "cpu", "cpu", "cpu", "display", "display" },
    memory = "one screen", note = "one more screen", tag = "smoothest",
  },
  [3] = {
    name = "Blitter copy", needs = "the GPU's display engine and its blitter", tag = "least processor",
    sentence = "As Page flips, and the GPU's blit engine does the copy into the hidden "
               .. "screen - the processor only composes.",
    stages = { "cpu", "cpu", "cpu", "gpu", "display", "display" },
    memory = "one screen", note = "the back buffer shared",
  },
  [4] = {
    name = "Blitter composing", needs = "the GPU's display engine and its blitter", tag = "experimental",
    sentence = "As Blitter copy, and windows' opaque interiors copied by the blitter from "
               .. "their own buffers. Less processor; on this GPU, fewer frames.",
    stages = { "cpu", "cpu", "gpu+cpu", "gpu", "display", "display" },
    memory = "one screen", note = "and windows' buffers shared",
  },
  [5] = {
    name = "GPU composing", needs = "the GPU's display engine and its 3D engine", tag = "not yet",
    sentence = "As Blitter composing, on the render engine: windows' uncovered interiors drawn "
               .. "by the 3D pipeline from their own buffers, and the copy to the screen too.",
    stages = { "cpu", "cpu", "gpu+cpu", "gpu", "display", "display" },
    memory = "one screen", note = "and windows' buffers shared", unbuilt = true,
  },
}

rendering.LAST = 4                      -- the highest that can be chosen today: 5 not
                                        -- until its frames are held right (`testing.md` 18.686)

-- A mode from a saved value: an integer in range, or the default.
function rendering.valid(m)
  if math.type(m) == "integer" and m >= 0 and m <= rendering.LAST then return m end
  return rendering.DEFAULT
end

function rendering.name(m)
  local mode = rendering.MODES[m]
  return mode and mode.name or ("mode " .. tostring(m))
end

return rendering
