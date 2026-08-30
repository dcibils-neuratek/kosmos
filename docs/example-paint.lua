-- apps/paint/manifest.lua
--
-- return {
--   name  = "Paint",
--   needs = { "ui", "gfx", "/home/images", "/dev/clock" },
-- }

local ui  = require("ui")
local fs  = require("fs")
local gfx = require("gfx")

local W, H = 480, 360          -- canvas size
local PALETTE_W = 140

local win = ui.window{ title = "Paint", w = PALETTE_W + W, h = H + 40 }

-- The canvas is a surface, not a Lua table.
-- gfx.surface asks the surface server for memory and returns a handle.
-- Pixels never cross the IPC boundary.
local canvas = gfx.surface(W, H)
canvas:fill(0xffffff)

--------------------------------------------------------------------
-- state
--------------------------------------------------------------------

local state = {
  tool     = "brush",
  color    = 0x000000,
  width    = 3,
  drawing  = false,
  last     = nil,          -- {x, y} of the previous event
  file     = nil,
}

local colors = {
  0x000000, 0xffffff, 0xe03131, 0xf59f00,
  0x2f9e44, 0x1971c2, 0x9c36b5, 0x868e96,
}

local tools = { "brush", "line", "rect", "bucket", "eraser" }

--------------------------------------------------------------------
-- undo: per-tile snapshots, not the whole canvas
--------------------------------------------------------------------

local undo = { stack = {}, max = 20 }

function undo.push(x, y, w, h)
  -- we store only the region we are about to touch
  local r = { x = x, y = y, w = w, h = h, data = canvas:copy(x, y, w, h) }
  table.insert(undo.stack, r)
  if #undo.stack > undo.max then table.remove(undo.stack, 1) end
end

function undo.pop()
  local r = table.remove(undo.stack)
  if not r then return end
  canvas:paste(r.data, r.x, r.y)
  win:invalidate()
end

local function stroke_region(x1, y1, x2, y2)
  local g = state.width + 2
  local x, y = math.min(x1, x2) - g, math.min(y1, y2) - g
  local w, h = math.abs(x2 - x1) + g * 2, math.abs(y2 - y1) + g * 2
  return math.max(0, x), math.max(0, y), math.min(W - x, w), math.min(H - y, h)
end

--------------------------------------------------------------------
-- tools
--
-- Lua decides what gets drawn and where. The pixel loop happens
-- inside the surface, in C. Never in a Lua for loop.
--------------------------------------------------------------------

local actions = {}

function actions.brush(x1, y1, x2, y2)
  undo.push(stroke_region(x1, y1, x2, y2))
  canvas:line(x1, y1, x2, y2, state.color, state.width)
end

function actions.eraser(x1, y1, x2, y2)
  undo.push(stroke_region(x1, y1, x2, y2))
  canvas:line(x1, y1, x2, y2, 0xffffff, state.width * 3)
end

function actions.line(x1, y1, x2, y2)
  undo.push(stroke_region(x1, y1, x2, y2))
  canvas:line(x1, y1, x2, y2, state.color, state.width)
end

function actions.rect(x1, y1, x2, y2)
  local x, y = math.min(x1, x2), math.min(y1, y2)
  local w, h = math.abs(x2 - x1), math.abs(y2 - y1)
  undo.push(stroke_region(x1, y1, x2, y2))
  canvas:rect(x, y, w, h, state.color, state.width)
end

function actions.bucket(x, y)
  -- flood fill over 120k pixels. In pure Lua this would be unacceptable.
  -- The surface does it in C and returns the bounding box it touched.
  undo.push(0, 0, W, H)
  canvas:flood(x, y, state.color)
end

--------------------------------------------------------------------
-- persistence
--------------------------------------------------------------------

local function save(name)
  name = name or state.file
  if not name then return end
  state.file = name

  local path = "/home/images/" .. name
  fs.write(path, canvas:export())        -- raw bytes, never passing through Lua

  fs.setattr(path, {
    width      = W,
    height     = H,
    modified   = fs.read("/dev/clock").epoch,
    color_count = canvas:count_colors(),
  })
end

local function open(name)
  local data = fs.read("/home/images/" .. name)
  canvas:import(data)
  state.file = name
  undo.stack = {}
  win:invalidate()
end

-- live query: the gallery updates on its own if another process
-- writes an image into the directory
local gallery = {}
fs.query("/home/images", "width > 0", function(r)
  gallery = r
  win:invalidate()
end)

--------------------------------------------------------------------
-- input
--------------------------------------------------------------------

local function in_canvas(x, y)
  return x >= PALETTE_W and y >= 40
end

local function to_canvas(x, y)
  return x - PALETTE_W, y - 40
end

win:on("mousedown", function(x, y)
  if in_canvas(x, y) then
    local cx, cy = to_canvas(x, y)
    state.drawing = true
    state.last = { cx, cy }

    if state.tool == "bucket" then
      actions.bucket(cx, cy)
      state.drawing = false
      win:invalidate()
    end
    return
  end

  -- color palette
  if y > 40 and y < 40 + 4 * 32 then
    local col = math.floor((x - 12) / 32) + 1
    local row = math.floor((y - 40) / 32)
    local i = row * 4 + col
    if colors[i] then state.color = colors[i]; win:invalidate() end
    return
  end

  -- tools
  local i = math.floor((y - 190) / 26) + 1
  if tools[i] then state.tool = tools[i]; win:invalidate() end
end)

win:on("mousemove", function(x, y)
  if not state.drawing then return end
  if state.tool == "line" or state.tool == "rect" then return end

  local cx, cy = to_canvas(x, y)
  local ux, uy = state.last[1], state.last[2]
  actions[state.tool](ux, uy, cx, cy)
  state.last = { cx, cy }
  win:invalidate()
end)

win:on("mouseup", function(x, y)
  if not state.drawing then return end
  local cx, cy = to_canvas(x, y)

  if state.tool == "line" or state.tool == "rect" then
    actions[state.tool](state.last[1], state.last[2], cx, cy)
    win:invalidate()
  end

  state.drawing = false
  state.last = nil
end)

win:on("key", function(k)
  if     k == "z"  then undo.pop()
  elseif k == "s"  then save(state.file or "untitled.raw")
  elseif k == "["  then state.width = math.max(1, state.width - 1); win:invalidate()
  elseif k == "]"  then state.width = math.min(40, state.width + 1); win:invalidate()
  end
end)

--------------------------------------------------------------------
-- window drawing
--
-- The UI is drawing commands (model B).
-- The canvas is a surface blit (the shared memory exception).
--------------------------------------------------------------------

win:on("draw", function(gc)
  gc:fill(0x2b2b2b)

  gc:text(12, 22, state.file or "untitled", 0xdddddd)
  gc:text(PALETTE_W + 12, 22,
          state.tool .. "  " .. state.width .. "px", 0x999999)

  -- palette
  for i, c in ipairs(colors) do
    local col, row = (i - 1) % 4, math.floor((i - 1) / 4)
    local x, y = 12 + col * 32, 40 + row * 32
    gc:rect(x, y, 26, 26, c)
    if c == state.color then
      gc:border(x - 2, y - 2, 30, 30, 0xffffff, 2)
    end
  end

  -- tools
  for i, t in ipairs(tools) do
    local y = 190 + (i - 1) * 26
    local color = (t == state.tool) and 0x4dabf7 or 0x888888
    gc:text(14, y, t, color)
  end

  -- gallery
  gc:text(14, 330, #gallery .. " images", 0x666666)

  -- the canvas: a single blit, no commands
  gc:blit(canvas, PALETTE_W, 40)
end)

win:on("close", function() save() end)
win:run()
