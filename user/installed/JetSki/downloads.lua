-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- JetSki's downloads panel, one of its files (`jetski.lua`, *JetSki's
-- files*): a function of `J`, what the window hands its parts, giving
-- back the panel and what the window calls - `close`, `toggle`, and
-- `frame`, every pass.
--
-- **The downloads** (`docs/webkit.html`, *Downloads*): what WebKit's
-- network process is writing into `/Home/Downloads`, or wrote, the newest
-- first - a row each, with how far it has got, and what can be done with
-- it: Cancel while it runs; Open, with the application its type names,
-- and Show, in Tracker, once it is there; Retry when it failed. A
-- notification says when one finishes, and pressing it opens the file.
--

return function(J)
  local ui, theme, files = J.ui, J.theme, J.files
  local notify = use("/Kosmos/Libraries/notify.lua")
  local filetypes = use("/Kosmos/Libraries/filetypes.lua")
  local DL_FOLDER = "/Home/Downloads"
  local dl = {}                        -- `web.downloads` fills it, reused
  local dl_n = 0
  local dl_seen = {}                   -- id -> the state last seen
  local dl_frames = 0
  local DL_ROW, DL_HEAD, DL_ROWS = 52, 44, 6

  local panel = ui.view{ w = 460, h = 10, hidden = true }

  local function amount(n)
    if not n then return "?" end
    if n < 1024 then return ("%d bytes"):format(n) end
    if n < 1024 * 1024 then return ("%.0f KB"):format(n / 1024) end
    if n < 1024 * 1024 * 1024 then return ("%.1f MB"):format(n / 1048576) end
    return ("%.2f GB"):format(n / 1073741824)
  end

  -- What a row says under its name.
  local function dl_words(d)
    local from = J.host_of(d.address)
    if d.state == "downloading" then
      local rate = d.seconds > 0.5 and (" - %s/s"):format(amount(d.received / d.seconds)) or ""
      if d.expected then
        return ("%s of %s%s"):format(amount(d.received), amount(d.expected), rate)
      end
      return amount(d.received) .. rate
    elseif d.state == "done" then
      return ("%s - %s"):format(from, amount(d.received))
    elseif d.state == "cancelled" then
      return "Cancelled"
    end
    return ("Failed: %s"):format(d.reason ~= "" and d.reason or ("error " .. d.error))
  end

  -- The actions a row has, right to left.
  local function dl_actions(d)
    if d.state == "downloading" then return { "Cancel" } end
    if d.state == "done" then return { "Show", "Open" } end
    return { "Retry" }
  end

  local function dl_open(d)
    local how = filetypes.how_to_open(d.path)
    if not how then
      J.say(("nothing opens %s - it is in %s"):format(d.name, DL_FOLDER))
      return
    end
    print(("jetski: opening download %s with %s"):format(d.path, how.program))
    fs.send("/Running/wm", { type = "launch", program = how.program, args = how.args, wait = false })
  end

  local function dl_show(d)
    local folder = d.path:match("^(.*)/[^/]*$") or DL_FOLDER
    print("jetski: showing download in " .. folder)
    fs.send("/Running/wm", { type = "launch", program = "tracker", args = files.quote(folder), wait = false })
  end

  function panel:draw(g)
    g:fill_round(0, 0, self.w, self.h, theme.line, 9)
    g:fill_round(1, 1, self.w - 2, self.h - 2, theme.window, 8)
    g:line_icon(14, 14, "downloads", theme.accent)
    g:text(38, 12, "Downloads", theme.text, nil, "ui")
    local into = "into " .. DL_FOLDER
    self.hits = {}
    local clear_x = self.w - 14 - gfx.measure("Clear")
    if dl_n > 0 then
      g:text(clear_x, 14, "Clear", theme.accent)
      self.hits[#self.hits + 1] = { x = clear_x - 4, y = 8, w = gfx.measure("Clear") + 8, h = 24, act = "Clear" }
    end
    g:text(clear_x - 16 - gfx.measure(into), 14, into, theme.text_dim)
    g:fill(1, DL_HEAD - 4, self.w - 2, 1, theme.line_soft)
    if dl_n == 0 then
      g:text(14, DL_HEAD + 10, "Nothing downloaded yet. A file a page cannot show comes here.", theme.text_dim)
      return
    end
    local y = DL_HEAD
    for i = 1, math.min(dl_n, DL_ROWS) do
      local d = dl[i]
      local x = self.w - 14
      for _, a in ipairs(dl_actions(d)) do
        local w = gfx.measure(a)
        x = x - w
        g:text(x, y + 10, a, theme.accent)
        self.hits[#self.hits + 1] = { x = x - 4, y = y, w = w + 8, h = DL_ROW - 6, act = a, d = d }
        x = x - 14
      end
      local room = x - 14 - 8
      local finished = d.state == "done"
      g:line_icon(14, y + 10, finished and "document" or "downloads",
                  d.state == "failed" and theme.text_dim or theme.text)
      g:text(38, y + 8, ui.fitted(d.name ~= "" and d.name or d.address, room - 24), theme.text, nil, "ui")
      g:text(38, y + 28, ui.fitted(dl_words(d), room - 24), theme.text_dim)
      if d.state == "downloading" then
        local w = room - 24
        local part = d.expected and d.expected > 0 and math.min(1, d.received / d.expected) or 0
        g:fill_round(38, y + DL_ROW - 8, w, 3, theme.line_soft, 1)
        g:fill_round(38, y + DL_ROW - 8, math.max(2, math.floor(w * part)), 3, theme.accent, 1)
      end
      y = y + DL_ROW
    end
  end

  function panel:mouse(action, x, y)
    if action ~= "press" then return true end
    for _, h in ipairs(self.hits or {}) do
      if x >= h.x and x < h.x + h.w and y >= h.y and y < h.y + h.h then
        print(("jetski: download %s%s"):format(h.act, h.d and (" " .. h.d.name) or ""))
        if h.act == "Clear" then
          for i = 1, dl_n do
            if dl[i].state ~= "downloading" then J.web.download_forget(dl[i].id) end
          end
        elseif h.act == "Cancel" then J.web.download_cancel(h.d.id)
        elseif h.act == "Open" then dl_open(h.d)
        elseif h.act == "Show" then dl_show(h.d)
        elseif h.act == "Retry" then
          J.web.download_forget(h.d.id)
          J.web.load(h.d.address)
        end
        dl_n = J.web.downloads(dl)
        self.h = DL_HEAD + math.max(1, math.min(dl_n, DL_ROWS)) * DL_ROW + 8
        return true
      end
    end
    return true
  end

  local function close()
    if panel.hidden then return false end
    panel.hidden = true
    return true
  end

  local function toggle()
    J.lock_close()
    J.shield_close()
    if close() then return end
    dl_n = J.web.downloads(dl)
    panel.h = DL_HEAD + math.max(1, math.min(dl_n, DL_ROWS)) * DL_ROW + 8
    panel.x = math.max(8, J.dlbutton.x + J.dlbutton.w - panel.w)
    panel.y = J.HEAD + 2
    panel.hidden = false
    print(("jetski: the downloads shown, %d, at %d,%d %dx%d"):format(dl_n,
          (J.win.origin_x or 0) + panel.x, (J.win.origin_y or 0) + panel.y, panel.w, panel.h))
  end

  -- Every frame: whether the downloads changed enough to draw again. The
  -- list is asked for when one began or ended, and a few times a second
  -- while one runs, never every frame.
  local function frame()
    local running = (J.st.downloading or 0) > 0
    if not J.st.downloads and not (running or J.dlbutton.part) then return false end
    dl_frames = dl_frames + 1
    if not J.st.downloads and dl_frames % 20 ~= 0 then return false end
    dl_n = J.web.downloads(dl)
    local got, all = 0, 0
    for i = 1, dl_n do
      local d = dl[i]
      if d.state == "downloading" then
        got = got + d.received
        all = all + (d.expected or math.max(d.received * 2, 1))
      end
      if dl_seen[d.id] ~= d.state then
        local was = dl_seen[d.id]
        dl_seen[d.id] = d.state
        if d.state == "downloading" and not was then
          J.say(("downloading %s"):format(d.name ~= "" and d.name or d.address))
        elseif d.state == "done" then
          J.say(("downloaded %s, %d bytes, into %s"):format(d.name, d.received, d.path))
          notify.post{ title = "Downloaded", body = d.name, open = d.path }
        elseif d.state == "failed" then
          J.say(("download of %s failed: %s"):format(d.name, dl_words(d)))
        end
      end
    end
    J.dlbutton.part = running and (all > 0 and got / all or 0) or nil
    if not panel.hidden then
      panel.h = DL_HEAD + math.max(1, math.min(dl_n, DL_ROWS)) * DL_ROW + 8
    end
    return true
  end

  return { panel = panel, close = close, toggle = toggle, frame = frame }
end
