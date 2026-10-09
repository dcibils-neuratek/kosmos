-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- **The composer**: a message written, in a window of its own, and handed
-- to `maild` to send (`docs/mail.md` M6, drawn in `docs/mail.html`).
--
--   local compose = use("/Kosmos/Libraries/mailcompose.lua")
--   local c = compose.open{ account = { address =, name = }, to = { ... },
--                           cc =, subject =, body =, in_reply_to =,
--                           references =, title = "Reply", replaces = }
--   c = compose.reopen(id)                        a draft kept in `Drafts here`
--   local spec = compose.answer(path, "reply" | "replyall" | "forward", own)
--   c:tend()                                      its events, its frame;
--                                                 false once it has closed
--
-- Ctrl+Return sends; closing it - its light, or Super+Q - keeps it as a draft.
--
-- To, Cc and Bcc, each a row of addresses become chips as they are typed
-- and completed from the mail kept (`addresses.lua`); the subject; and the
-- body - Text Editor's page, `docview`, wrapped, with its caret, selection
-- and undo - sent as plain text in UTF-8 (`docs/mail.md`: writing with style
-- waits for Write's typing to be a kit).
--
-- **A library and not Mail's own**: whatever wants a message written - a
-- file sent from Tracker, a page from the browser - opens this, and `maild`
-- sends it, so there is one composer in the system. It runs inside the
-- process that opens it: Mail polls it beside its own window, as Cafesa3D
-- polls its Render window.
--
-- **Kept as it is written**: what is typed is in `Drafts here` within two
-- seconds of the last key, so nothing written is lost to a crash; closed,
-- the draft's copy goes to the account's Drafts on the server, by `maild`.
-- **Sent by `maild`**: the message is written into the Outbox and `maild`
-- asked, so it is sent when this window has gone, and the window closes the
-- moment Send is pressed.

local compose = {}

local ui = use("/Kosmos/Libraries/ui.lua")
local wmproto = use("/Kosmos/Libraries/wmproto.lua")
local keys = use("/Kosmos/Libraries/keys.lua")
local files = use("/Kosmos/Libraries/files.lua")
local regions = use("/Kosmos/Libraries/regions.lua")
local clock = use("/Kosmos/Libraries/clock.lua")
local docview = use("/Kosmos/Libraries/docview.lua")
local addresses = use("/Kosmos/Libraries/addresses.lua")
local mailkit = use("/Kosmos/Kits/mail")
local pk = use("/Kosmos/Libraries/pixelkit.lua").new(ui)

local theme = ui.theme
local L = ui.layout

local HOME = "/Home/Mail"
local OUTBOX = HOME .. "/Outbox"
local DRAFTS = HOME .. "/Drafts here"

local counter_hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000

-- The drawing's numbers: a label column of 70, rows ruled beneath, chips
-- as pills.
local EDGE, LABEL_W, ROW_H, CHIP_H = 18, 70, 38, 24
local SUGGEST_W, SUGGEST_ROW = 300, 44
local QUIET_SECONDS = 2

local FIELDS = { "to", "cc", "bcc", "subject" }
local LABELS = { to = "To", cc = "Cc", bcc = "Bcc", subject = "Subject" }

local C = {}
C.__index = C

--------------------------------------------------------------------------
-- Addresses, as typed and as kept.
--------------------------------------------------------------------------

-- "Lena Moreau <lena@example.com>", '"Moreau, Lena" <lena@...>' or a bare
-- address; nil when it is none of them.
local function address_of(text)
  text = tostring(text or ""):gsub("^%s+", ""):gsub("%s+$", "")

  local name, addr = text:match("^(.-)%s*<([^<>%s]+)>$")

  if not addr then addr, name = text, "" end

  name = name:gsub('^"(.*)"$', "%1")

  if not addr:match("^[^%s@<>,;]+@[^%s@<>,;]+%.[^%s@<>,;]+$") then return nil end

  return { name = name, address = addr }
end

local function written(who)
  return who.name ~= "" and ("%s <%s>"):format(who.name, who.address) or who.address
end

local function random_id()
  local ok, crypto = pcall(use, "/Kosmos/Kits/crypto")
  local bytes = ok and crypto and crypto.random(10)

  if not bytes then
    bytes = tostring(sys.ticks()) .. tostring(math.random(1, 1 << 30))
  end

  return (bytes:gsub(".", function(c) return ("%02x"):format(c:byte()) end)):sub(1, 20)
end

--------------------------------------------------------------------------
-- Making a composer.
--------------------------------------------------------------------------

local function plain_list(list)
  local out = {}

  for _, who in ipairs(list or {}) do
    local a = type(who) == "table" and who.address and { name = who.name or "", address = who.address }
              or address_of(who)

    if a then out[#out + 1] = a end
  end

  return out
end

--
-- `spec` = { account = { address, name }, to, cc, bcc (lists of addresses
-- or text), subject, body, in_reply_to, references, title, id, replaces =
-- { mailbox, uid } - a draft from the server this one takes the place of }.
--
function compose.open(spec)
  local screen = gfx.screen()
  local sw, sh = 1280, 800

  if screen then sw, sh = screen:size() end

  local W, H = math.min(680, sw - 40), math.min(560, sh - 80)
  local c = setmetatable({
    account = spec.account, id = spec.id or random_id(),
    title = spec.title or "New Message",
    rows = { to = { chips = plain_list(spec.to), text = "" },
             cc = { chips = plain_list(spec.cc), text = "" },
             bcc = { chips = plain_list(spec.bcc), text = "" },
             subject = { text = spec.subject or "" } },
    in_reply_to = spec.in_reply_to, references = spec.references, replaces = spec.replaces,
    reopened = spec.reopened,
    focus = #plain_list(spec.to) == 0 and "to" or "body",
    W = W, H = H, decode = keys.decoder(), dirty = true, changed = false,
    choice = 1, suggestions = {}, said = nil,
  }, C)

  c.win = ui.window{ title = c.title, w = W, h = H, direct = true, header = true, resizable = true,
                     x = spec.x, y = spec.y, centre = spec.x == nil }

  if not c.win or not c.win:surface() then
    print("mail: no window to write in")
    return nil
  end

  c.page = docview.new(ui, { x = 0, y = 0, w = W, h = H, text = spec.body or "", wrap = true,
                             column = 400, ground = theme.window,
                             on_change = function() c:edited() end })
  c.book = addresses.gather({ c.account and c.account.address })
  c.send_b = { text = "Send", go = true }
  c.attach_b = { icon = "attachment", disabled = true }    -- M7
  c.focused_at = sys.ticks()

  print(("mail: composer %s open, %s"):format(c.id, c.title))
  c:save()
  return c
end

-- A draft kept here, opened again where it was left.
function compose.reopen(id, place)
  local d = fs.read(DRAFTS .. "/" .. tostring(id) .. ".draft")

  if type(d) ~= "table" then return nil end

  d.id, d.reopened = id, true
  if place then d.x, d.y = place.x, place.y end
  return compose.open(d)
end

--------------------------------------------------------------------------
-- Reply, Reply All, Forward: a composer's beginning from a message.
--------------------------------------------------------------------------

local function plain_text_of(m)
  local plain, html

  for _, p in ipairs(m:parts()) do
    if not p.multipart and p.disposition ~= "attachment" and p.name == "" then
      if p.type == "text/plain" and not plain then plain = p end
      if p.type == "text/html" and not html then html = p end
    end
  end

  local p = plain or html

  if not p then return "" end

  local r = regions.make(math.max(p.bound, 1))

  if not r then return "" end

  local n = m:part_into(p.id, r.at, r.size) or 0
  local text = n > 0 and sys.region_read(r.cap, 0, n) or ""

  regions.free(r)

  if p == html then
    text = text:gsub("<[Ss][Tt][Yy][Ll][Ee].-</[Ss][Tt][Yy][Ll][Ee]>", "")
               :gsub("<[Bb][Rr]%s*/?>", "\n"):gsub("</?[Pp][^>]*>", "\n\n")
               :gsub("</?[Dd][Ii][Vv][^>]*>", "\n"):gsub("<[^>]*>", "")
               :gsub("&nbsp;", " "):gsub("&lt;", "<"):gsub("&gt;", ">")
               :gsub("&quot;", '"'):gsub("&#39;", "'"):gsub("&amp;", "&")
               :gsub("\n\n\n+", "\n\n")
  end

  return (text:gsub("\r\n", "\n"):gsub("%s+$", ""))
end

local function prefixed(subject, word)
  subject = tostring(subject or "")

  if subject:lower():match("^" .. word:lower() .. ":") then return subject end

  return word .. ": " .. subject
end

local function bracketed(id)
  id = tostring(id or ""):match("^%s*<?([^<>%s]+)>?%s*$")
  return id
end

--
-- **A reply's beginning**: whom it goes to - the sender, or Reply-To when
-- the message has one; with Reply All, everyone else it went to but this
-- person - the subject with its Re:, the message quoted beneath a line
-- saying who wrote it and when, and the headers that keep a conversation
-- together (In-Reply-To and References, RFC 5322 3.6.4). Forward has the
-- message beneath its own header and goes to nobody yet.
--
function compose.answer(path, kind, own)
  local r, size = regions.read_whole(path)

  if not r then return nil end

  local m = mailkit.parse(r.at, size)

  if not m then
    regions.free(r)
    return nil
  end

  local mine = {}

  for _, a in ipairs(own or {}) do mine[tostring(a):lower()] = true end

  local from = m:addresses("from")[1] or { name = "", address = "" }
  local reply_to = m:addresses("reply-to")
  local subject = m:header("subject") or ""
  local id = bracketed(m:header("message-id"))
  local refs = m:header("references") or ""
  local when = m:date()
  local text = plain_text_of(m)
  local spec = { references = nil }

  local who = from.name ~= "" and from.name or from.address
  local day = when and clock.at(when) or nil
  local on = day and ("On %d %s %d, %s wrote:"):format(day.day,
               ({ "January", "February", "March", "April", "May", "June", "July", "August",
                  "September", "October", "November", "December" })[day.month], day.year, who)
             or (who .. " wrote:")

  if kind == "forward" then
    local to = {}

    for _, a in ipairs(m:addresses("to")) do to[#to + 1] = written(a) end

    spec.title = "Forward"
    spec.subject = prefixed(subject, "Fwd")
    spec.body = "\n\n---------- Forwarded message ----------\n"
                .. "From: " .. written(from) .. "\n"
                .. (m:header("date") and ("Date: " .. m:header("date") .. "\n") or "")
                .. "Subject: " .. subject .. "\n"
                .. (#to > 0 and ("To: " .. table.concat(to, ", ") .. "\n") or "")
                .. "\n" .. text .. "\n"
  else
    local to, cc, seen = {}, {}, {}
    local function take(list, a)
      local k = a.address:lower()

      if not seen[k] and not mine[k] then
        seen[k] = true
        list[#list + 1] = a
      end
    end

    for _, a in ipairs(#reply_to > 0 and reply_to or { from }) do take(to, a) end

    if kind == "replyall" then
      for _, a in ipairs(m:addresses("to")) do take(to, a) end
      for _, a in ipairs(m:addresses("cc")) do take(cc, a) end
    end

    spec.title = kind == "replyall" and "Reply All" or "Reply"
    spec.to, spec.cc = to, cc
    spec.subject = prefixed(subject, "Re")
    spec.in_reply_to = id
    spec.references = id and ((refs ~= "" and (refs .. " ") or "") .. "<" .. id .. ">") or (refs ~= "" and refs or nil)
    local quoted = {}

    for line in (text .. "\n"):gmatch("(.-)\n") do
      quoted[#quoted + 1] = line == "" and ">" or ("> " .. line)
    end

    spec.body = "\n\n" .. on .. "\n" .. table.concat(quoted, "\n") .. "\n"
  end

  regions.free(r)
  return spec
end

--
-- **A draft from the Drafts mailbox, written on**: the one this machine
-- keeps when it is one of its own - its id is the Message-ID's first half -
-- or, from another computer, its fields and text read from the message,
-- the server's copy to be taken away when this one is sent or kept.
--
function compose.continue(path, mailbox, uid, account, place)
  local r, size = regions.read_whole(path)

  if not r then return nil end

  local m = mailkit.parse(r.at, size)
  local id = m and tostring(m:header("message-id") or ""):match("^%s*<?([%w%-]+)@") or nil
  local spec

  if id and fs.getattr(DRAFTS .. "/" .. id .. ".draft") then
    regions.free(r)
    return compose.reopen(id, place)
  end

  if m then
    spec = { account = account, to = m:addresses("to"), cc = m:addresses("cc"),
             subject = m:header("subject") or "", body = plain_text_of(m),
             in_reply_to = bracketed(m:header("in-reply-to")), references = m:header("references"),
             replaces = { mailbox = mailbox, uid = uid }, title = "Draft" }
    if place then spec.x, spec.y = place.x, place.y end
  end

  regions.free(r)
  return spec and compose.open(spec) or nil
end

--------------------------------------------------------------------------
-- Keeping it: the draft, and the message.
--------------------------------------------------------------------------

-- The text typed into a row and not yet a chip, made chips; what is not an
-- address is left as text, and its row named.
function C:settle(name)
  local row = self.rows[name]

  if not row.chips or row.text:match("^%s*$") then return true end

  local left = {}

  for piece in (row.text .. ","):gmatch("([^,;]*)[,;]") do
    if not piece:match("^%s*$") then
      local a = address_of(piece)

      if a then row.chips[#row.chips + 1] = a else left[#left + 1] = piece end
    end
  end

  row.text = table.concat(left, ", "):gsub("^%s+", "")
  return #left == 0
end

function C:state()
  local out = { account = self.account, title = self.title, subject = self.rows.subject.text,
                body = self.page:content(), in_reply_to = self.in_reply_to,
                references = self.references, replaces = self.replaces }

  for _, name in ipairs({ "to", "cc", "bcc" }) do
    local list = {}

    for _, a in ipairs(self.rows[name].chips) do list[#list + 1] = a end
    if self.rows[name].text ~= "" then list[#list + 1] = self.rows[name].text end
    out[name] = list
  end

  return out
end

function C:empty()
  local s = self:state()

  return #s.to == 0 and #s.cc == 0 and #s.bcc == 0 and s.subject == ""
         and s.body:match("^%s*$") ~= nil
end

function C:edited()
  self.changed = true
  self.quiet_from = sys.ticks()
  self.dirty = true
end

-- What is typed, kept in `Drafts here`.
function C:save()
  files.make_folder(DRAFTS)
  fs.write(DRAFTS .. "/" .. self.id .. ".draft", self:state())
  self.changed = false
end

-- The message as it would be sent, into `path`; true, or nil and why.
function C:build(path)
  local s = self:state()
  local now = clock.now()
  local domain = tostring(self.account and self.account.address or ""):match("@(.+)$") or "kosmos"
  local spec = { from = { name = self.account.name or "", address = self.account.address },
                 to = self.rows.to.chips, cc = self.rows.cc.chips, subject = s.subject,
                 text = s.body, date = now and now.epoch or 0, zone = clock.offset(),
                 id = self.id .. "@" .. domain, in_reply_to = self.in_reply_to,
                 references = self.references }
  local room = #s.body * 3 + 16384
  local r = regions.make(room)

  if not r then return nil, "no memory to write it in" end

  local n, need = mailkit.build(spec, r.at, r.size)

  if not n then
    regions.free(r)
    r = regions.make(need)
    if not r then return nil, "no memory to write it in" end
    n = mailkit.build(spec, r.at, r.size)
  end

  local ok, why = regions.write_file(path, r, n)

  regions.free(r)
  return ok, why
end

local function ask(req)
  local ok, reply = pcall(fs.send, "/Running/maild", req)

  return ok and type(reply) == "table" and reply.ok, ok and type(reply) == "table" and reply.why or nil
end

--
-- **Send**: every address made a chip, the message written into the Outbox
-- with its envelope - everyone it goes to, the Bcc among them - and
-- `maild` asked. The draft goes: it is a message now.
--
function C:send()
  for _, name in ipairs({ "to", "cc", "bcc" }) do
    if not self:settle(name) then
      self.said = ("\u{201c}%s\u{201d} is not an address"):format(self.rows[name].text)
      self.focus = name
      return false
    end
  end

  local rcpt = {}

  for _, name in ipairs({ "to", "cc", "bcc" }) do
    for _, a in ipairs(self.rows[name].chips) do rcpt[#rcpt + 1] = a.address end
  end

  if #rcpt == 0 then
    self.said = "Who is it to?"
    self.focus = "to"
    return false
  end

  files.make_folder(OUTBOX)

  local ok, why = self:build(OUTBOX .. "/" .. self.id .. ".eml")

  if not ok then
    self.said = "Not written: " .. tostring(why)
    return false
  end

  fs.write(OUTBOX .. "/" .. self.id .. ".send", { account = self.account.address, rcpt = rcpt,
           subject = self.rows.subject.text, draft = self.id, replaces = self.replaces })
  files.remove(DRAFTS .. "/" .. self.id .. ".draft")
  self.sent = true

  local asked, no = ask{ type = "send", name = self.id }

  print(("mail: composer %s sent to %d, maild %s"):format(self.id, #rcpt,
        asked and "has it" or ("not running: " .. tostring(no))))
  self:close()
  return true
end

-- Closed: sent, or kept as a draft - here, and on the server by `maild` -
-- or, when nothing was written, thrown away.
function C:close()
  if self.closed then return end

  self.closed = true

  if not self.sent then
    if self:empty() then
      files.remove(DRAFTS .. "/" .. self.id .. ".draft")
      -- A draft opened again and emptied: its copy on the server goes too.
      if self.reopened or self.replaces then
        ask{ type = "undraft", account = self.account.address, name = self.id, replaces = self.replaces }
      end
      print(("mail: composer %s closed, nothing kept"):format(self.id))
    else
      self:save()

      local eml = DRAFTS .. "/" .. self.id .. ".eml"

      if self:build(eml) then
        ask{ type = "draft", account = self.account.address, name = self.id }

        -- A draft from another computer, kept now as this one's: its old
        -- copy goes once the new one is there.
        if self.replaces then
          ask{ type = "undraft", account = self.account.address, replaces = self.replaces }
          self.replaces = nil
          self:save()
        end
      end

      print(("mail: composer %s closed, kept as a draft"):format(self.id))
    end
  end

  if self.win and self.win.running then self.win:close() end
end

--------------------------------------------------------------------------
-- Drawing.
--------------------------------------------------------------------------

-- A row's chips and text laid out in lines across `w`: each chip's place,
-- where the typing goes, and how tall the row is.
local function lay_row(row, x0, w)
  local x, line, placed = x0, 0, {}

  for i, a in ipairs(row.chips) do
    local label = a.name ~= "" and a.name or a.address
    local cw = math.min(gfx.measure(label) + 20, w)

    if x > x0 and x + cw > x0 + w then x, line = x0, line + 1 end
    placed[i] = { x = x, line = line, w = cw, label = ui.fitted(label, cw - 20, "ui") }
    x = x + cw + 6
  end

  local tw = gfx.measure(row.text) + 4

  if x > x0 and x + math.max(tw, 60) > x0 + w then x, line = x0, line + 1 end

  return placed, x, line, ROW_H + line * (CHIP_H + 6)
end

function C:layout()
  local W = self.W
  local y = L.head
  local x0, fw = EDGE + LABEL_W + 8, W - (EDGE + LABEL_W + 8) - EDGE

  self.places = {}

  for _, name in ipairs(FIELDS) do
    local row = self.rows[name]
    local p = { name = name, x = 0, y = y, w = W, fx = x0, fw = fw }

    if row.chips then
      p.chips, p.tx, p.lines, p.h = lay_row(row, x0, fw)
    else
      p.tx, p.lines, p.h = x0, 0, ROW_H
    end

    self.places[name] = p
    y = y + p.h
  end

  self.body_at = { x = 0, y = y, w = W, h = math.max(40, self.H - y) }
  self.page.x, self.page.y, self.page.w, self.page.h = 0, 0, W, self.body_at.h

  local three = (self.win.lights and self.win.lights.w) or 68

  self.send_b.w = pk.button_width("Send")
  self.send_b.x = W - L.lights_in - three - L.head_edge - self.send_b.w
  self.send_b.y = pk.centre(31)
  self.attach_b.x, self.attach_b.y = self.send_b.x - 36, pk.centre(26)

  local where = ("to %d,%d cc %d,%d bcc %d,%d subject %d,%d body %d,%d send %d,%d")
    :format(self.places.to.fx, self.places.to.y + 19, self.places.cc.fx, self.places.cc.y + 19,
            self.places.bcc.fx, self.places.bcc.y + 19, self.places.subject.fx,
            self.places.subject.y + 19, 40, self.body_at.y + 20,
            self.send_b.x + self.send_b.w // 2, self.send_b.y + 15)

  if where ~= self.said_where then
    self.said_where = where
    print(("mail: composer %s places %s"):format(self.id, where))
  end
end

function C:draw()
  local s = self.win:surface()

  if not s then return false end

  self:layout()
  s:fill(0, 0, self.W, self.H, theme.window)

  local subject = self.rows.subject.text

  pk.header(s, 0, 0, self.W, self.title, subject ~= "" and subject or nil, self.attach_b.x - 12)
  self.send_b.pressed = false
  pk.button(s, self.send_b)
  pk.iconbutton(s, self.attach_b)

  local gh = gfx.height()

  for _, name in ipairs(FIELDS) do
    local p, row = self.places[name], self.rows[name]
    local focused = self.focus == name
    local ty = p.y + (ROW_H - gh) // 2

    s:text(EDGE, ty, LABELS[name], theme.text_dim, nil, "ui")

    for _, chip in ipairs(p.chips or {}) do
      local cy = p.y + (ROW_H - CHIP_H) // 2 + chip.line * (CHIP_H + 6)

      s:fill_round(chip.x, cy, chip.w, CHIP_H, theme.mix(theme.window, theme.accent, 220), CHIP_H // 2)
      s:text(chip.x + 10, cy + (CHIP_H - gh) // 2, chip.label, theme.text, nil, "ui")
    end

    local line_y = ty + (p.lines or 0) * (CHIP_H + 6)
    local text = ui.fitted(row.text, p.fx + p.fw - p.tx, "ui", true)

    s:text(p.tx, line_y, text, theme.text, nil, "ui")

    if focused then
      s:fill(p.tx + gfx.measure(text) + 1, line_y - 1, 2, gh + 2, theme.accent)
    end

    s:fill(0, p.y + p.h - 1, self.W, 1, theme.line_soft)
  end

  self.page.focused = self.focus == "body"
  ui.paint_view(self.page, s, self.body_at.x, self.body_at.y)

  if self.said then
    local w = gfx.measure(self.said) + 24
    local x, y = (self.W - w) // 2, self.H - 44

    s:fill_round(x, y, w, 30, theme.mix(theme.window, 0xffd04040, 200), 8)
    s:text(x + 12, y + (30 - gh) // 2, self.said, theme.text, nil, "ui")
  end

  self:draw_suggestions(s)

  return self.win:commit{ x = 0, y = 0, w = self.W, h = self.H }
end

-- The addresses the typing could be, under the row being typed in.
function C:draw_suggestions(s)
  local p = self.places[self.focus]
  local list = self.suggestions

  self.suggest_rows = {}

  if not (p and p.chips) or #list == 0 then return end

  local x = math.min(p.tx, self.W - SUGGEST_W - EDGE)
  local y = p.y + p.h - 2
  local h = #list * SUGGEST_ROW + 8
  local gh = gfx.height()

  s:fill_round(x + 2, y + 3, SUGGEST_W, h, theme.mix(theme.window, 0xff000000, 60), 10)
  s:fill_round(x, y, SUGGEST_W, h, theme.raised or theme.sunken, 10)
  s:frame_round(x, y, SUGGEST_W, h, theme.line_soft, 10)

  for i, e in ipairs(list) do
    local ry = y + 4 + (i - 1) * SUGGEST_ROW

    if i == self.choice then
      s:fill_round(x + 4, ry, SUGGEST_W - 8, SUGGEST_ROW, theme.mix(theme.window, theme.accent, 220), 7)
    end

    local name = e.name ~= "" and e.name or e.address
    local sub = ("%s \u{00b7} %d %s"):format(e.address, e.count, e.count == 1 and "message" or "messages")

    s:text(x + 14, ry + 5, ui.fitted(name, SUGGEST_W - 28, "ui"), theme.text, nil, "ui")
    s:text(x + 14, ry + 7 + gh, ui.fitted(sub, SUGGEST_W - 28, "ui"), theme.text_dim, nil, "ui")
    self.suggest_rows[i] = { x = x, y = ry, w = SUGGEST_W, h = SUGGEST_ROW, entry = e }
  end

  local said = {}

  for i, e in ipairs(list) do said[i] = e.address end

  local line = table.concat(said, " ")

  if line ~= self.said_suggest then
    self.said_suggest = line
    print("mail: composer suggests " .. line)
  end
end

--------------------------------------------------------------------------
-- Typing and pressing.
--------------------------------------------------------------------------

function C:suggest()
  local row = self.rows[self.focus]

  self.suggestions = {}
  self.choice = 1

  if not (row and row.chips) or row.text:match("^%s*$") then return end

  local taken = {}

  for _, n in ipairs({ "to", "cc", "bcc" }) do
    for _, a in ipairs(self.rows[n].chips) do taken[a.address:lower()] = true end
  end

  for _, e in ipairs(addresses.match(self.book, row.text, 8)) do
    if not taken[e.address] and #self.suggestions < 5 then self.suggestions[#self.suggestions + 1] = e end
  end
end

function C:take_suggestion(i)
  local e = self.suggestions[i]
  local row = self.rows[self.focus]

  if not (e and row and row.chips) then return false end

  row.chips[#row.chips + 1] = { name = e.name, address = e.address }
  row.text = ""
  self.suggestions = {}
  self:edited()
  return true
end

local ORDER = { "to", "cc", "bcc", "subject", "body" }

function C:move_focus(by)
  local i = 1

  for k, n in ipairs(ORDER) do
    if n == self.focus then i = k end
  end

  if self.rows[self.focus] then self:settle(self.focus) end

  self.focus = ORDER[(i - 1 + by) % #ORDER + 1]
  self.suggestions = {}
end

function C:key(c)
  local k, mods = keys.parts(c)
  local ctrl = (mods & keys.CTRL) ~= 0
  local shift = (mods & keys.SHIFT) ~= 0

  self.said = nil

  if ctrl and (k == keys.ENTER or k == 10) then
    self:send()
    return true
  end


  if self.focus == "body" then
    if k == keys.TAB and shift then self:move_focus(-1) return true end

    return self.page:key(c)
  end

  local row = self.rows[self.focus]

  if k == keys.TAB then
    if #self.suggestions > 0 and not shift then self:take_suggestion(self.choice) end
    self:move_focus(shift and -1 or 1)
  elseif (k == keys.ENTER or k == 10) and row.chips then
    if #self.suggestions > 0 then
      self:take_suggestion(self.choice)
    else
      self:settle(self.focus)
      if row.text == "" then self:move_focus(1) end
    end
  elseif k == keys.ENTER or k == 10 then
    self:move_focus(1)
  elseif k == keys.ESCAPE then
    self.suggestions = {}
  elseif (k == keys.DOWN or k == keys.UP) and #self.suggestions > 0 then
    self.choice = (self.choice - 1 + (k == keys.DOWN and 1 or -1)) % #self.suggestions + 1
  elseif (k == 8 or k == 127) and row.text == "" and row.chips and #row.chips > 0 then
    table.remove(row.chips)
    self:edited()
  elseif k == 8 or k == 127 then
    row.text = row.text:sub(1, (utf8.offset(row.text, -1) or 1) - 1)
    self:edited()
    self:suggest()
  elseif row.chips and (k == 44 or k == 59) then        -- , and ; end an address
    self:settle(self.focus)
    self.suggestions = {}
    self:edited()
  elseif k >= 32 and not ctrl then
    row.text = row.text .. utf8.char(k)
    self:edited()
    self:suggest()
  else
    return false
  end

  return true
end

function C:paste_text(text)
  text = tostring(text or ""):gsub("[\r\n]+", " ")

  local row = self.rows[self.focus]

  if not row then return false end

  row.text = row.text .. text
  if row.chips then self:settle(self.focus) end
  self:edited()
  return true
end

function C:press(x, y)
  self.said = nil

  if pk.inside(self.send_b, x, y) then
    self:send()
    return
  end

  for i, r in ipairs(self.suggest_rows or {}) do
    if pk.inside(r, x, y) then
      self:take_suggestion(i)
      return
    end
  end

  if y < L.head then
    self.win:take_hold(x, y)
    return
  end

  for _, name in ipairs(FIELDS) do
    local p = self.places[name]

    if y >= p.y and y < p.y + p.h then
      if self.rows[self.focus] then self:settle(self.focus) end
      self.focus = name
      self.suggestions = {}
      return
    end
  end

  if y >= self.body_at.y then
    if self.rows[self.focus] then self:settle(self.focus) end
    self.focus = "body"
    self.suggestions = {}
    self.holding = true
    self.page:mouse("press", x - self.body_at.x, y - self.body_at.y)
  end
end

--
-- **Its turn**: the window's events, a draft kept when the typing has
-- paused, and a frame when something changed. False once it has closed.
--
function C:tend()
  if self.closed then return false end

  local reply = wmproto.poll(self.win.handle, 0)

  if not reply then
    self:close()
    return false
  end

  for _, ev in ipairs(reply.events or {}) do
    if self.win:direct_event(ev) then
      self.dirty = true
    elseif ev.type == "resize" then
      self.W, self.H = ev.w, ev.h
      self.dirty = true
    elseif ev.type == "close" then
      self:close()
      return false
    elseif ev.type == "theme" then
      self.page.ground = theme.window
      self.dirty = true
    elseif ev.type == "key" then
      local a, b = self.decode(ev.code)

      for _, ch in ipairs({ a, b }) do
        if self:key(ch) then self.dirty = true end
        if self.closed then return false end
      end
    elseif ev.type == "paste" then
      if self.focus == "body" then
        self.page:edit("paste")
      else
        self:paste_text(wmproto.paste())
      end

      self.dirty = true
    elseif ev.type == "selectall" or ev.type == "copy" or ev.type == "cut" then
      if self.focus == "body" and self.page:edit(ev.type) then self.dirty = true end
    elseif ev.type == "wheel" then
      if (ev.y or 0) >= self.body_at.y then self.page:wheel(ev.n or 0) end
      self.dirty = true
    elseif ev.type == "mouse" and not ev.menu then
      if ev.action == "press" and ev.button ~= "right" then
        self:press(ev.x or 0, ev.y or 0)
        self.dirty = true
      elseif self.holding and (ev.action == "move" or ev.action == "release") then
        self.page:mouse(ev.action, (ev.x or 0) - self.body_at.x, (ev.y or 0) - self.body_at.y)
        if ev.action == "release" then self.holding = false end
        self.dirty = true
      end
    end

    if self.closed then return false end
  end

  -- What was typed, kept once the typing pauses.
  if self.changed and sys.ticks() - (self.quiet_from or 0) > QUIET_SECONDS * counter_hz then
    self:save()
  end

  if self.dirty then
    self.dirty = false
    if not self:draw() then
      self:close()
      return false
    end
  end

  return not self.closed
end

return compose
