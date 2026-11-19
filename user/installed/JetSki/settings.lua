-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- JetSki's Settings, one of its files (`jetski.lua`, *JetSki's files*):
-- a function of `J`, giving back Settings' view and `fill`, which lays
-- its cards out again from what is set.
--
-- **Settings** (`docs/webkit.html`, *Settings*; `roadmap.md`, S1): a tab
-- of JetSki's own at `about:settings` - a view of the kit's that loads
-- nothing, which this page is drawn over - in the kit's cards, two columns
-- when there is room. Every
-- change is kept the moment it is made, and reaches every tab at once.
-- History, cookies, and Privacy and Ad Blocking are S2 to S4.
--

return function(J)
  local ui, prefs, theme = J.ui, J.prefs, J.theme

  local view = ui.view{ x = 0, y = J.view.y, w = J.width(), h = J.view.h, hidden = true }
  view.scroll, view.tall = 0, 0

  function view:draw(g)
    g:fill(0, 0, self.w, self.h, theme.window)
  end

  function view:wheel(n)
    local most = math.max(0, self.tall - self.h)
    self.scroll = math.max(0, math.min(most, self.scroll - n * 48))
    for _, c in ipairs(self.children) do c.y = c.top - self.scroll end
    return true
  end

  J.win:add(view)

  -- What Settings says of pages, handed to the kit: every view's.
  local function apply_pages()
    J.webkit.settings{ scripts = J.setting.javascript, images = J.setting.images }
  end

  local function fill()
    local page = view
    page.children = {}

    local function save(key, value, after)
      J.keep(key, value)
      print(("jetski: set %s %s"):format(key, tostring(value)))
      if after then after(value) end
    end

    local function switch(key, after)
      return ui.switch{ on = J.setting[key], on_change = function(_, on) save(key, on, after) end }
    end

    local function choice(key, after)
      return ui.dropdown{ choices = prefs.CHOICES[key], value = J.setting[key],
                          on_change = function(_, v) save(key, v, after) end }
    end

    local home = ui.field{ text = J.setting.home, w = 260 }
    home.on_change = function(_, text)
      if text ~= "" and not text:find("%c") and text ~= J.setting.home then save("home", text) end
    end

    local zoom = choice("zoom", function(v) J.webkit.zoom(v) end)
    local images = switch("images", apply_pages)
    local javascript = switch("javascript", apply_pages)
    local search = choice("search")

    local groups = {
      { name = "Starting", rows = {
          { label = "Home page", control = home },
          { label = "When JetSki opens", control = choice("opens") } } },
      { name = "Pages", rows = {
          { label = "Zoom", note = "All of the page: its words, pictures and boxes", control = zoom },
          { label = "Load images", control = images },
          { label = "JavaScript",
            note = "Off makes most sites plain or blank; for reading, or a page that misbehaves",
            control = javascript } } },
      { name = "Searching", rows = {
          { label = "Search with", note = "Words in the address field that are not an address",
            control = search } } },
    }

    local two = page.w >= 860
    local columns = two and { { groups[1], groups[2] }, { groups[3] } } or { groups }
    local each = page.w // #columns

    page.tall = 0
    for i, list in ipairs(columns) do
      local c = ui.cards{ x = (i - 1) * each, y = 0, w = each, h = 4000, groups = list, width = 440 }
      c.h = c.content_h + 16
      c.top = 0
      page:add(c)
      page.tall = math.max(page.tall, c.h)
    end
    page.scroll = math.max(0, math.min(page.scroll, page.tall - page.h))
    for _, c in ipairs(page.children) do c.y = c.top - page.scroll end

    -- Where its controls are in the window, for whoever drives it from
    -- outside (`x86-jetski`).
    local function at(v)
      local x, y = v:in_window()
      return ("%d,%d"):format(x + v.w // 2, y + v.h // 2)
    end
    print(("jetski: settings, %d columns, javascript %s, images %s, zoom %s, search %s")
          :format(#columns, at(javascript), at(images), at(zoom), at(search)))
  end

  return { view = view, fill = fill }
end
