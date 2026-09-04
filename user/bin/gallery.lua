-- kosmos: application
-- kosmos: section demos
-- The widget gallery. Every control the kit has, in one window.
--
--   wm gallery
--
--   click          focus and operate a control
--   Tab            move the focus
--   Enter / Space  activate
--   arrows         move in a list
--
-- Started by `wm`, which hands it /app/wm and nothing else it did not
-- already have. It draws by sending commands; the pixels stay in the
-- window manager, which is why this window would survive this program
-- hanging.

local ui = use("/lib/ui.lua")
-- The *kit's* palette, not a copy of it.
--
-- `use` runs the chunk again and hands back a different table, and only the
-- one `ui.lua` holds is the one it mutates when the desktop changes theme.
-- An application that loaded its own kept the colours it started with while
-- every widget around it changed - which is exactly what Monitor, Processes,
-- Photo and the Terminal did.
local theme = ui.theme

local win, err = ui.window{ title = "gallery", w = 460, h = 330,
                            x = 60, y = 90 }

if not win then
  print("gallery: " .. tostring(err))
  return
end

local status = ui.label{ x = 16, y = 296,
                         text = "click, or Tab and Enter",
                         color = "text_dim" }

local function say(text)
  status.text = text
end

win:add(ui.label{ x = 16, y = 14, text = "Widgets", color = "text" })
win:add(ui.label{ x = 16, y = 32,
                  text = "BeOS's vocabulary, this system's finish",
                  color = "text_dim" })

win:add(ui.button{ x = 16, y = 60, text = "Press me",
                   on_click = function() say("the button was pressed") end })

win:add(ui.button{ x = 150, y = 60, text = "And me",
                   on_click = function() say("the other one") end })

win:add(ui.checkbox{ x = 16, y = 104, text = "a checkbox",
                     on_change = function(_, on)
                       say(on and "checked" or "unchecked")
                     end })

win:add(ui.field{ x = 16, y = 130, w = 240, text = "editable text",
                  on_enter = function(_, t) say("entered: " .. t) end })

win:add(ui.list{ x = 16, y = 172, w = 240, h = 112,
                 items = { "threads", "address spaces", "endpoints",
                           "capabilities", "and nothing else" },
                 on_select = function(_, item) say("chose " .. item) end })

-- A panel on the right, to show that a view clips its children: the label
-- inside it is wider than it is and is cut off at the edge rather than
-- drawn over the list.
local panel = win:add(ui.view{ x = 276, y = 60, w = 168, h = 224 })

function panel:draw(g)
  g:fill(0, 0, self.w, self.h, "raised")
  g:frame(0, 0, self.w, self.h, "line")
  g:text(8, 8, "a nested view", "text")
  g:text(8, 28, "its children are clipped to it,", "text_dim")
  g:text(8, 44, "and draw in its coordinates", "text_dim")
end

panel:add(ui.label{ x = 8, y = 76,
                    text = "this label is far wider than the panel holding it",
                    color = "good" })

win:add(status)
win:run()
