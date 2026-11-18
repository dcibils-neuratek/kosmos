-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- JetSki's stopped page, one of its files (`jetski.lua`, *JetSki's
-- files*): a function of `J`, giving back the page's view, `frame`,
-- every pass, `now`, the crash shown or nil, and `look_up`, an
-- address's function by the Debug Kit.
--
-- **A tab whose page stopped** (`docs/webkit.html`, drawn 14 November
-- 2026): its process ended, so there is no picture, and this is drawn in
-- the page's place instead - with **What happened** under it, how Nebula
-- recorded the death, because a development build should say (Diego: "it
-- should help us work in bugs and edge cases easily", "right now we are
-- blind on whats going in"). `crash` is `web.crash`'s table for the tab
-- shown, or nil while its page is alive.
--

return function(J)
  local ui, theme = J.ui, J.theme

  local crash = nil
  local gone = ui.view{ x = 0, y = J.view.y, w = J.width(), h = J.view.h, hidden = true }
  local MONO_LH = gfx.height("mono") + 3
  local LABEL_W = 112

  local function hex(n) return n and ("0x%x"):format(n) or "?" end

  -- The fault in words: which kind of access, and whether the page was there.
  local function fault_words(c)
    if c.fault == "page fault" then              -- x86: the error code's bits
      local d = c.detail
      local what = d & 16 ~= 0 and "an instruction fetch"
                   or (d & 2 ~= 0 and "a write" or "a read")
      return ("page fault: %s, %s"):format(what, d & 1 ~= 0 and "not allowed" or "not present")
    elseif c.fault and c.fault:match("abort") then -- ARM: the syndrome's ISS
      local iss = c.detail & 0x1ffffff
      local status = iss & 0x3f
      local what = c.fault:match("instruction") and "an instruction fetch"
                   or (iss & 64 ~= 0 and "a write" or "a read")
      local why = (status >= 4 and status <= 7) and "not present"
                  or ((status >= 12 and status <= 15) and "not allowed" or ("status 0x%x"):format(status))
      return ("%s: %s, %s"):format(c.fault, what, why)
    end
    return c.fault or "?"
  end

  -- What an address is, as far as can be told from the record alone.
  local function address_words(c)
    local a = c.addr
    if a < 4096 then return hex(a) .. " - a null pointer" end
    if c.sp and math.abs(a - c.sp) < 64 * 1024 then
      return ("%s - %d bytes from thread %d's stack pointer"):format(hex(a), a - c.sp, c.thread)
    end
    return hex(a)
  end

  -- **What happened**, as label and value: what is drawn, and what Copy
  -- details puts on the clipboard.
  -- **Which function** (`docs/debug.md`): the Debug Kit reads this image's
  -- symbols - the web process runs this same image, so a fault's instruction
  -- is an address in the file beside this one. Looked up once per address,
  -- the pass after the stopped page is first drawn, so it never waits for
  -- the 7 MB of symbols to be read; the kit's table given back after.
  local have_debug, debugkit = pcall(use, "/Kosmos/Kits/debug")
  if not have_debug or type(debugkit) ~= "table" then debugkit = nil end
  local IMAGE = J.WEBKIT .. "/webkit.elf"
  local named = {}                     -- an address -> its function's name, or false

  local function look_up(pc)
    if named[pc] == nil and debugkit then
      local name, offset = debugkit.name(IMAGE, pc)
      named[pc] = name and ("%s + 0x%x"):format(name, offset) or false
      if not name then print("jetski: no function's name for " .. hex(pc) .. ": " .. tostring(offset)) end
      debugkit.forget()
    end
    return named[pc]
  end

  local function function_of(pc)
    local n = named[pc]
    if n == nil then return debugkit and "looking it up" or "no Debug Kit here" end
    return n or "no symbol holds it"
  end

  local function crash_rows(c)
    local rows = { { "process", ("web, %d"):format(c.pid) } }
    if not c.known then
      rows[#rows + 1] = { "fault", "not collected yet" }
    elseif c.fault then
      rows[#rows + 1] = { "fault", fault_words(c) }
      rows[#rows + 1] = { "address", address_words(c) }
      rows[#rows + 1] = { "at", hex(c.pc) .. " in webkit.elf" }
      rows[#rows + 1] = { "in", function_of(c.pc) }
      rows[#rows + 1] = { "thread", ("%d, stack pointer %s"):format(c.thread, hex(c.sp)) }
    else
      rows[#rows + 1] = { "ended", ("with code %d, not by a fault"):format(c.code or 0) }
    end
    if c.address ~= "" then rows[#rows + 1] = { "loading", c.address } end
    return rows
  end

  local function crash_details(c)
    local b = sys.build and sys.build() or {}
    local out = { ("JetSki, Kosmos %s - a page stopped"):format(b.version or "?") }
    for _, r in ipairs(crash_rows(c)) do
      out[#out + 1] = ("%-9s %s"):format(r[1], r[2])
    end
    return table.concat(out, "\n") .. "\n"
  end

  local gone_reload = ui.button{ text = "Reload", go = true, on_click = function()
    J.web.reload()
  end }
  local gone_close = ui.button{ text = "Close tab", on_click = function()
    for _, t in ipairs(J.tabs) do
      if t.shown then J.close_tab(t.id) return end
    end
  end }
  local gone_copy = ui.button{ text = "Copy details", on_click = function()
    if crash then
      use("/Kosmos/Libraries/wmproto.lua").copy(crash_details(crash))
      print("jetski: the page's details copied")
    end
  end }

  gone:add(gone_reload)
  gone:add(gone_close)
  gone:add(gone_copy)

  -- Where everything on it goes, from its size: one column, in the middle.
  local GONE_COL = 600

  local function gone_top()
    local rows = crash and #crash_rows(crash) or 0
    local tall = 40 + 32 + 28 + 26 + 30 + 18 + 30 + rows * MONO_LH + 12
    return math.max(20, (gone.h - tall) // 2)
  end

  local function lay_gone()
    local top = gone_top() + 40 + 32 + 28 + 26
    local widths = gone_reload.w + gone_close.w + gone_copy.w + 16
    local x = (gone.w - widths) // 2
    gone_reload.x, gone_reload.y = x, top
    gone_close.x, gone_close.y = x + gone_reload.w + 8, top
    gone_copy.x, gone_copy.y = gone_close.x + gone_close.w + 8, top
  end

  function gone:draw(g)
    g:fill(0, 0, self.w, self.h, theme.window)
    if not crash then return end
    lay_gone()

    local y = gone_top()
    local mid = self.w // 2
    local function centred(text, color, role, px, at)
      local tw = gfx.measure(text, role and px and ui.sized(role, px) or nil)
      g:text(mid - tw // 2, at, text, color, theme.window, role, px)
    end

    g:fill_round(mid - 16, y, 32, 32, theme.line_soft, 16)
    g:fill_round(mid - 12, y + 4, 24, 24, theme.window, 12)
    y = y + 40
    centred("This page stopped", theme.text, "ui", 18, y)
    y = y + 32
    centred(("The page at %s ended unexpectedly. Your other tabs are fine."):format(
            J.host_of(crash.address)), theme.text_dim, nil, nil, y)

    -- What happened, in a box under the buttons.
    local rows = crash_rows(crash)
    local bw = math.min(GONE_COL, self.w - 32)
    local bx = mid - bw // 2
    local by = gone_reload.y + gone_reload.h + 18
    local bh = 30 + #rows * MONO_LH + 12
    g:fill_round(bx, by, bw, bh, theme.line, 8)
    g:fill_round(bx + 1, by + 1, bw - 2, bh - 2, theme.field or theme.window, 7)
    g:text(bx + 12, by + 8, "What happened", theme.text, nil)
    local b = sys.build and sys.build() or {}
    local tag = ("Kosmos %s - development"):format(b.version or "?")
    g:text(bx + bw - 12 - gfx.measure(tag), by + 8, tag, theme.text_dim, nil)
    g:fill(bx + 1, by + 28, bw - 2, 1, theme.line)

    local ry = by + 36
    for _, r in ipairs(rows) do
      g:text(bx + 12, ry, r[1], theme.text_dim, nil, "mono")
      g:text(bx + 12 + LABEL_W, ry, ui.fitted(r[2], bw - 24 - LABEL_W, "mono"),
             theme.text, nil, "mono")
      ry = ry + MONO_LH
    end
  end

  J.win:add(gone)

  -- **Every pass**: the tab shown's page stopped, or stopped and now
  -- collected, or loading again - whichever it is, the stopped page
  -- follows. True when what is drawn changed.
  local function frame()
    local st, page, changed = J.st, J.page, false
    local was = crash
    -- A process not yet collected is asked about again, for a second or two
    -- of passes, and then left as it is.
    if st.crashed or st.started or st.tab ~= page.crash_tab then
      page.crash_asks = 120
    end
    if page.crash_asks > 0 and (st.crashed or st.started or st.tab ~= page.crash_tab
                                or (crash and not crash.known)) then
      page.crash_asks = page.crash_asks - 1
      crash = J.web.crash(st.tab)
      page.crash_tab = st.tab
    end
    if crash ~= was then
      J.cover()
      if crash and (not was or crash.known ~= was.known) then
        J.say("the page's process ended")
        for _, r in ipairs(crash_rows(crash)) do
          print(("jetski: %-9s %s"):format(r[1], r[2]))
        end
      end
      changed = true
    end
    -- The function, the pass after the stopped page first showed.
    if crash and crash.fault and named[crash.pc] == nil then
      if page.naming == crash.pc then
        print("jetski: in        " .. tostring(look_up(crash.pc) or "no symbol holds it"))
        changed = true
      else
        page.naming = crash.pc
      end
    end
    return changed
  end

  return { view = gone, frame = frame, now = function() return crash end, look_up = look_up }
end
