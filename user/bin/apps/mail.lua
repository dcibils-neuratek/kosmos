-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Mail
-- kosmos: name Mail
-- kosmos: section applications
-- kosmos: needs keyring-mail
--
-- Kosmos Mail: mailboxes down the side, the messages in the middle, the one
-- chosen on the right (`docs/mail.md` M4; the windows `docs/mail.html`,
-- agreed by Diego on 8 October 2026: "as recommended, go ahead and build
-- it").
--
--   mail                    the accounts in /Home/Mail, as `maild` keeps them
--   mail --add              Add Account, at once
--
-- **The thinnest layer** (`CLAUDE.md`, kits supply and applications
-- orchestrate): `maild` speaks to the servers and keeps each message as a
-- file with its facts as attributes; the Mail Kit reads a message; Write's
-- engine (`pageset`, `pagedraw`) sets its text on a page as wide as the pane
-- and as tall as the text. This decides what is shown and what a press
-- does - and **never speaks IMAP, and never waits on the network**.
--
-- **What a person does is shown at once and sent after**: a message read,
-- flagged, archived or deleted changes here in the frame it was pressed,
-- and `maild` is asked to make the server agree. What `maild` learns comes
-- back through `/Temporary/maild/status`, read on this window's own clock.
--
-- A window that draws its own pixels (`pixelkit`), as Maps and Write are:
-- a message's page is drawn into a surface of its own and copied in.
--
-- **An HTML message is drawn as its sender drew it** (M5): by the browser's
-- engine, with no scripts and no forms; a picture sent inside it (`cid:`)
-- shown at once, from the Mail Kit's part; a picture on the network not
-- fetched until Load Pictures is pressed, because fetching one tells its
-- sender the message was read. Links open in the browser.
--
-- **Writing** (M6) is the composer's (`mailcompose.lua`), a window of its
-- own for each message, polled here beside this one: New Message, Reply,
-- Reply All and Forward open one, and a draft pressed in Drafts opens in
-- one. What it writes `maild` sends; the Outbox's state is in the header.

local ui = use("/Kosmos/Libraries/ui.lua")
local wmproto = use("/Kosmos/Libraries/wmproto.lua")
local keys = use("/Kosmos/Libraries/keys.lua")
local files = use("/Kosmos/Libraries/files.lua")
local regions = use("/Kosmos/Libraries/regions.lua")
local clock = use("/Kosmos/Libraries/clock.lua")
local richtext = use("/Kosmos/Libraries/richtext.lua")
local writedoc = use("/Kosmos/Libraries/writedoc.lua")
local pageset = use("/Kosmos/Libraries/pageset.lua")
local pagedraw = use("/Kosmos/Libraries/pagedraw.lua")
local faces = use("/Kosmos/Libraries/faces.lua")
local pk = use("/Kosmos/Libraries/pixelkit.lua").new(ui)
local mailkit = use("/Kosmos/Kits/mail")
local webpictures = use("/Kosmos/Libraries/webpictures.lua")
local http = use("/Kosmos/Libraries/http.lua")
local compose = use("/Kosmos/Libraries/mailcompose.lua")

-- **The browser's engine draws an HTML message** (M5): parsed, laid out at
-- the paper's width and painted into it as a page is. An image built
-- without it (`FULL=0`) shows the message as its text, and says so.
local web_found, web = pcall(use, "/Kosmos/Kits/web")

if not web_found or type(web) ~= "table" then web = nil end

-- The engine set up once with its own default stylesheets, as the browser
-- sets it up; without them it lays nothing out, and the message is text.
if web then
  local sheet = sys.asset("netsurf/default.css")

  if not (web.setup and sheet and web.setup(sheet, sys.asset("netsurf/quirks.css")) == true) then
    print("mail: the browser's engine would not set up; HTML is shown as text")
    web = nil
  end
end
local theme = ui.theme
local L = ui.layout

local HOME = "/Home/Mail"
local STATUS = "/Temporary/maild/status"

local counter_hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000

--------------------------------------------------------------------------
-- The window.
--------------------------------------------------------------------------

local screen = gfx.screen()
local sw, sh = 1280, 800

if screen then sw, sh = screen:size() end

local W = math.min(1220, sw - 60)
local H = math.min(740, sh - 100)

local win = ui.window{ title = "Mail", w = W, h = H, direct = true, header = true,
                       resizable = true, centre = true }

if not win or not win:surface() then
  print("mail: no window")
  return
end

local SIDE_W, LIST_W = 230, 360
local ROW_H = 76
local sidebar = true

-- The paper a message is set on: the drawing's, light in every look, as a
-- letter is.
local PAPER = 0xfff7f7f5
local INK = "#22252b"
local QUOTE_INK = "#5b6677"
local LINK = "#2a55c9"

-- Points to pixels: 96 to the inch on a screen, 72 points to it.
local SCALE = 4 / 3

--------------------------------------------------------------------------
-- What `maild` says, and the accounts as they are kept.
--------------------------------------------------------------------------

local status = { accounts = {}, version = -1 }
local accounts = {}             -- { address, name, boxes = { ... } }
local current = nil             -- { account, box } - the mailbox shown
local said = ""                 -- the list's line under the mailbox's name

-- The mailboxes in the drawing's order: what each is for, then the rest by
-- name.
local ORDER = { inbox = 1, drafts = 2, sent = 3, archive = 4, junk = 5, trash = 6 }
local USE_ICON = { inbox = "inbox", drafts = "page", sent = "sent", archive = "archive",
                   junk = "junk", trash = "trash", all = "archive", flagged = "flag",
                   important = "star" }

--
-- **A mailbox as a person reads it** (the M700, 8 October: Gmail's came out
-- as `[Gmail]/Sent Mail` and `[Airmail]/Done`): its last name, indented
-- under its parent - and Gmail's own `[Gmail]`, which holds Gmail's
-- mailboxes and cannot be opened, left out of the path and the list. A
-- mailbox kept for a use is named by its last name alone and comes first.
--
local function shaped(b)
  local path = tostring(b.title or b.name):gsub("^%[Gmail%]/", "")
  local parts = {}

  for part in path:gmatch("[^/]+") do parts[#parts + 1] = part end

  b.path = path
  b.label = b.use == "inbox" and "Inbox" or parts[#parts] or path
  b.depth = b.use and 0 or math.max(0, #parts - 1)

  return b
end

local function sorted_boxes(boxes)
  local out = {}

  for _, b in ipairs(boxes or {}) do
    if b.selectable ~= false and tostring(b.name) ~= "[Gmail]" then
      out[#out + 1] = shaped(b)
    end
  end

  table.sort(out, function(x, y)
    local ox, oy = ORDER[x.use] or (x.use and 50) or 99, ORDER[y.use] or (y.use and 50) or 99

    if ox ~= oy then return ox < oy end
    return x.path:lower() < y.path:lower()
  end)

  return out
end

--
-- The accounts: `maild`'s status when it is running, and what each
-- account's folder keeps when it is not - so a window opened with `maild`
-- stopped still shows the mail already kept.
--
local said_boxes = nil

local function read_accounts()
  local from_status = {}

  for _, a in ipairs(status.accounts or {}) do from_status[a.address] = a end

  local out = {}

  for _, name in ipairs(fs.list(HOME) or {}) do
    local account = fs.read(HOME .. "/" .. name .. "/account")

    if type(account) == "table" and account.imap then
      local st = from_status[account.address or name] or {}
      local boxes = st.boxes

      if not boxes or #boxes == 0 then
        boxes = {}

        for _, b in ipairs(fs.read(HOME .. "/" .. name .. "/mailboxes") or {}) do
          boxes[#boxes + 1] = { name = b.name, title = b.title, use = b.use,
                                folder = HOME .. "/" .. name .. "/" .. (b.title or b.name),
                                selectable = b.selectable }
        end
      end

      out[#out + 1] = { address = account.address or name, name = account.name,
                        host = account.imap.host, state = st.state, why = st.why,
                        boxes = sorted_boxes(boxes) }
    end
  end

  table.sort(out, function(x, y) return x.address < y.address end)
  accounts = out

  -- Said when they change, as a person reads them: a dash a level down.
  local words = {}

  for _, acc in ipairs(accounts) do
    for _, b in ipairs(acc.boxes) do
      words[#words + 1] = ("-"):rep(b.depth or 0) .. tostring(b.label)
    end
  end

  words = table.concat(words, ", ")

  if words ~= said_boxes and words ~= "" then
    said_boxes = words
    print("mail: mailboxes " .. words)
  end
end

local function account_of(address)
  for _, a in ipairs(accounts) do
    if a.address == address then return a end
  end

  return nil
end

local function box_of(a, name)
  for _, b in ipairs(a and a.boxes or {}) do
    if b.name == name then return b end
  end

  return nil
end

local function box_for(a, use)
  for _, b in ipairs(a and a.boxes or {}) do
    if b.use == use then return b end
  end

  return nil
end

--------------------------------------------------------------------------
-- Asking `maild`: after the frame that shows what was asked.
--------------------------------------------------------------------------

local outbox = {}
local launched = false

local function ask(req)
  outbox[#outbox + 1] = req
end

-- `maild` started if it is not running: by the window manager, as the
-- Deskbar would, so it outlives this window.
local function start_maild()
  if fs.getattr("/Running/maild") or launched then return end

  launched = true
  fs.send("/Running/wm", { type = "launch", program = "maild", wait = false })
  print("mail: maild started")
end

local function send_asked()
  if #outbox == 0 then return end

  if not fs.getattr("/Running/maild") then
    start_maild()
    return                          -- kept, for when it has its name
  end

  local list = outbox

  outbox = {}

  for _, req in ipairs(list) do
    local reply, why = fs.send("/Running/maild", req)

    if not (reply and reply.ok) then
      print(("mail: maild did not take %s: %s"):format(tostring(req.type),
            tostring(reply and reply.why or why)))
    end
  end
end

--------------------------------------------------------------------------
-- The list: a mailbox's messages, newest first.
--------------------------------------------------------------------------

local rows = {}                 -- { uid, path }, newest first
local shown = {}                -- `rows` as the search leaves them
local facts = {}                -- path -> attributes, read once
local top = 1                   -- the first row drawn
local chosen = nil              -- the uid on the right
local query = ""
local listing = ""              -- the folder's names, to know it changed

local function facts_of(row)
  local f = facts[row.path]

  if f == nil then
    f = fs.getattr(row.path) or {}
    facts[row.path] = f
  end

  return f
end

local function matches(row)
  if query == "" then return true end

  local f = facts_of(row)
  local q = query:lower()

  for _, v in ipairs({ f.from, f.from_address, f.subject, f.preview }) do
    if tostring(v or ""):lower():find(q, 1, true) then return true end
  end

  return false
end

local function filter()
  shown = {}

  for _, r in ipairs(rows) do
    if matches(r) then shown[#shown + 1] = r end
  end

  top = math.max(1, math.min(top, #shown))
end

--
-- **Newest first by UID**, which IMAP gives in the order messages arrived:
-- so a mailbox opens without reading any message's attributes but the
-- ones on the screen (`docs/mail.md`, "the list file measured at M4").
--
local function read_rows(keep_facts)
  rows = {}

  if not current then
    filter()
    return
  end

  local folder = current.box.folder
  local names = fs.list(folder) or {}

  listing = table.concat(names, "\0")

  for _, name in ipairs(names) do
    local uid = tonumber(name:match("^(%d+)%.eml$"))

    if uid then rows[#rows + 1] = { uid = uid, path = folder .. "/" .. name } end
  end

  table.sort(rows, function(x, y) return x.uid > y.uid end)

  if not keep_facts then facts = {} end

  filter()
end

--------------------------------------------------------------------------
-- A message: read by the Mail Kit, its text set by Write's engine.
--------------------------------------------------------------------------

local catalogue = faces.catalogue(gfx.typefaces())
local measure = faces.measure(catalogue, gfx.typeface)
local drawer = pagedraw.new(measure)

local STYLES = {
  { name = "Body", face = "IBM Plex Sans", weight = "Regular", size_pt = 11,
    colour = INK, spacing_lines = 1.25, after_pt = 8, next = "Body" },
  { name = "Quote", face = "IBM Plex Sans", weight = "Regular", size_pt = 11,
    colour = QUOTE_INK, spacing_lines = 1.2, after_pt = 8, indent_left_mm = 4,
    next = "Body" },
}

local styles, by_name = richtext.styles(STYLES, writedoc.STYLES)

local message = nil             -- the one on the right, read
local scroll = 0                -- how far down its page
local band = nil                -- the page's visible part, drawn
local band_for = nil            -- what `band` was drawn for

--
-- Text's lines as paragraphs: a blank line ends one, lines inside one keep
-- their breaks, and `>` lines are quotes. Addresses on the web are runs in
-- the link colour, and kept, so a press on one opens it.
--
local URL = "https?://[%w%-%._~:/%?#%[%]@!%$&'%(%)%*%+,;=%%]+"

local function paragraphs(text)
  local out, links = {}, {}
  local lines, kind = {}, nil

  local function flush()
    if #lines == 0 then return end

    local body = table.concat(lines, "\n")
    local runs, at, found = {}, 1, {}

    while true do
      local s, e = body:find(URL, at)

      if not s then break end

      -- A sentence's full stop or a bracket around an address is not in it.
      while e > s and body:sub(e, e):match("[%.,;:%)%]]") do e = e - 1 end

      if s > at then runs[#runs + 1] = { text = body:sub(at, s - 1) } end

      runs[#runs + 1] = { text = body:sub(s, e), colour = LINK, underline = true }
      found[#found + 1] = { from = s, to = e, url = body:sub(s, e) }
      at = e + 1
    end

    if at <= #body then runs[#runs + 1] = { text = body:sub(at) } end

    out[#out + 1] = richtext.paragraph({ style = kind, runs = runs }, by_name, "Body")
    links[#out] = found
    lines, kind = {}, nil
  end

  for line in (text .. "\n"):gmatch("(.-)\r?\n") do
    local quoted = line:match("^%s*>")
    local this = quoted and "Quote" or "Body"

    if line:match("^%s*$") then
      flush()
    else
      if kind and kind ~= this then flush() end

      kind = this
      lines[#lines + 1] = quoted and line:gsub("^%s*>%s?", "") or line
    end
  end

  flush()

  if #out == 0 then out[1] = richtext.paragraph({ style = "Body", runs = {} }, by_name, "Body") end

  return out, links
end

-- HTML as text, until HTML is drawn (M5): the tags taken out, its blocks
-- as lines, its few entities undone.
local function html_text(html)
  local t = html:gsub("<[Ss][Tt][Yy][Ll][Ee].-</[Ss][Tt][Yy][Ll][Ee]>", "")
                :gsub("<[Ss][Cc][Rr][Ii][Pp][Tt].-</[Ss][Cc][Rr][Ii][Pp][Tt]>", "")
                :gsub("<[Bb][Rr]%s*/?>", "\n")
                :gsub("</?[Pp][^>]*>", "\n\n")
                :gsub("</?[Dd][Ii][Vv][^>]*>", "\n")
                :gsub("</?[Tt][Rr][^>]*>", "\n")
                :gsub("<[^>]*>", "")
                :gsub("&nbsp;", " "):gsub("&lt;", "<"):gsub("&gt;", ">")
                :gsub("&quot;", '"'):gsub("&#39;", "'"):gsub("&amp;", "&")
                :gsub("[ \t]+", " "):gsub("\n[ \t]+", "\n"):gsub("\n\n\n+", "\n\n")

  return t
end

-- A part's content as a string: into a region by the kit, read out once.
local function part_text(m, p)
  local r = regions.make(math.max(p.bound, 1))

  if not r then return "" end

  local n = m:part_into(p.id, r.at, r.size) or 0
  local text = n > 0 and sys.region_read(r.cap, 0, n) or ""

  regions.free(r)

  return text or ""
end

local function read_message(row)
  local r, size = regions.read_whole(row.path)

  if not r then return nil end

  local m = mailkit.parse(r.at, size)

  if not m then
    regions.free(r)
    return nil
  end

  local from = m:addresses("from")[1] or { name = "", address = "" }
  local to = {}

  for _, who in ipairs(m:addresses("to")) do
    to[#to + 1] = who.name ~= "" and who.name or who.address
  end

  local plain, html, attachments = nil, nil, {}

  for _, p in ipairs(m:parts()) do
    if not p.multipart then
      local attached = p.disposition == "attachment" or p.name ~= ""

      if attached then
        attachments[#attachments + 1] = { name = p.name ~= "" and p.name or p.type,
                                          bytes = p.bytes }
      elseif p.type == "text/plain" and not plain then
        plain = p
      elseif p.type == "text/html" and not html then
        html = p
      end
    end
  end

  local text, as_html = "", false
  local html_source = html and part_text(m, html) or nil

  if plain then
    text = part_text(m, plain)
  elseif html_source then
    text, as_html = html_text(html_source), true
  end

  -- The pictures sent inside it, by their Content-ID, for its HTML to
  -- name - sixteen megabytes of them at most.
  local cids, cid_bytes = {}, 0

  if html_source and web then
    for _, p in ipairs(m:parts()) do
      if not p.multipart and p.cid ~= "" and cid_bytes + p.bytes < 16 * 1024 * 1024 then
        local bytes = part_text(m, p)

        cids[p.cid] = bytes
        cid_bytes = cid_bytes + #bytes
      end
    end
  end

  local out = {
    uid = row.uid, path = row.path,
    from = from.name ~= "" and from.name or from.address, address = from.address,
    to = table.concat(to, ", "), subject = m:header("subject") or "",
    date = m:date(), attachments = attachments,
    -- Drawn as HTML when the engine is here; its text otherwise.
    html = web and html_source or nil, cids = cids, tried = {},
    as_html = as_html and not web,
  }

  out.body, out.links = paragraphs(text)

  regions.free(r)

  return out
end

-- The paper's place in the window: inside the message pane, under its head.
local HEAD_H = 104

local function pane()
  local x = (sidebar and SIDE_W or 0) + LIST_W

  return x, L.head, W - x, H - L.head
end

local function paper_box()
  local x, y, w, h = pane()
  local top_y = y + HEAD_H + (message and #message.attachments > 0 and 44 or 0)
               + (message and (message.as_html or message.bar) and 34 or 0)

  return x + 20, top_y + 10, math.max(80, w - 40), math.max(40, y + h - top_y - 20)
end

-- The message's text set on one page as wide as the paper.
local function set_message()
  if not message then return end

  local _, _, pw = paper_box()
  local width_mm = pw / SCALE * 25.4 / 72

  if message.width_mm == width_mm and message.set then return end

  local doc = {
    format = writedoc.FORMAT, version = writedoc.VERSION,
    paper = { name = "Custom", width_mm = width_mm, height_mm = 297, landscape = false },
    margins_mm = { top = 6, bottom = 8, left = 7, right = 7 },
    header = { on = false, from_top_mm = 9, text = "" },
    footer = { on = false, from_bottom_mm = 6, page_numbers = false },
    facing = false, hyphenation = false, ligatures = true, language = "en-us",
    styles = styles, body = message.body, comments = {},
  }

  local t0 = sys.ticks()

  message.text_doc = doc
  message.set = pageset.set(doc, measure, nil, { endless = true })
  message.width_mm = width_mm
  band_for = nil

  print(("mail: set %d paragraphs in %.1f ms, the page %d px tall"):format(
        #message.body, (sys.ticks() - t0) * 1000 / counter_hz,
        math.floor(message.set.pages[1].height_pt * SCALE)))
end

local function shows_html()
  return message and message.html and not message.as_text
end

local function page_px()
  if shows_html() then return message.html_h or 0 end

  if not (message and message.set) then return 0 end

  return math.floor(message.set.pages[1].height_pt * SCALE + 0.5)
end

local function clamp_scroll()
  local _, _, _, ph = paper_box()

  scroll = math.max(0, math.min(scroll, page_px() - ph))
end

--------------------------------------------------------------------------
-- An HTML message: laid out by the browser's engine at the paper's width.
--------------------------------------------------------------------------

local function unescaped(cid)
  return (cid:gsub("%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end))
end

-- The pictures on the network it names and has not got.
local function remote_pictures()
  local list = {}

  for k, o in ipairs(message.page:ns_objects()) do
    local url = tostring(o.url or "")

    if not o.arrived and url:match("^https?:") then list[#list + 1] = { k = k, url = url } end
  end

  return list
end

local function lay_out_html(pw, ph)
  if not message.page then
    local t0 = sys.ticks()
    local doc, why = web.parse(message.html, "utf-8")

    if not doc then
      print("mail: its HTML would not parse: " .. tostring(why))
      message.html, message.as_html = nil, true
      return false
    end

    message.page = doc
    message.pics = webpictures.new(web, pw, ph)
    message.laid_w = nil
    message.parse_ms = (sys.ticks() - t0) * 1000 / counter_hz
  end

  if message.laid_w == pw and not message.relayout then return true end

  local t0 = sys.ticks()

  message.pics.page_w, message.pics.view_h = pw, ph

  local tall, why = message.page:ns_layout(pw, ph, "about:blank")

  if not tall then
    print("mail: its HTML could not be laid out: " .. tostring(why))
    message.html, message.as_html = nil, true
    return false
  end

  -- Its own pictures, at once: then laid out again, as a picture the page
  -- gave no size to takes its own.
  local handed = 0

  for k, o in ipairs(message.page:ns_objects()) do
    local cid = tostring(o.url or ""):match("^cid:(.+)$")

    if cid and not o.arrived and not message.tried[k] then
      message.tried[k] = true

      local bytes = message.cids[cid] or message.cids[unescaped(cid)]

      if bytes and message.pics:hand(message.page, k, bytes) then handed = handed + 1 end
    end
  end

  if handed > 0 or message.relayout then
    tall = message.page:ns_layout(pw, ph, "about:blank") or tall
    message.pics:to_boxes(message.page)
  end

  message.html_h = tall or 0
  message.laid_w, message.relayout = pw, false
  message.version = (message.version or 0) + 1

  local far = #remote_pictures()

  message.bar = (far > 0 and not message.loading)
                and ("%d %s from the network not loaded, so its sender cannot see it was read")
                    :format(far, far == 1 and "picture" or "pictures")
                or nil

  print(("mail: html laid out at %d in %.1f ms, %d px tall, %d sent inside, %d on the network")
        :format(pw, (sys.ticks() - t0) * 1000 / counter_hz, message.html_h, handed, far))

  return true
end

-- Load Pictures: those on the network fetched beside the window's own
-- loop, a pass at a time, so nothing here waits on the network.
local fetching = nil

local function load_pictures()
  if not (message and message.page) or fetching then return end

  local list = remote_pictures()

  if #list == 0 then return end

  local urls = {}

  for i, w in ipairs(list) do urls[i] = w.url end

  local m = message

  m.loading, m.bar = true, ("Loading %d pictures\u{2026}"):format(#list)
  fetching = { message = m, list = list,
               co = coroutine.create(function()
                 return http.get_many(urls, { pause = function() coroutine.yield() end })
               end) }
  print(("mail: loading %d pictures"):format(#list))
end

-- One pass of the fetch; true when it ended and the page changed.
local function step_fetch()
  if not fetching then return false end

  local ok, results = coroutine.resume(fetching.co)

  if ok and coroutine.status(fetching.co) ~= "dead" then return false end

  local f = fetching

  fetching = nil

  local m = f.message
  local came = 0

  if ok and m.page then
    for i, w in ipairs(f.list) do
      local reply = results and results[i] and results[i][1]

      if reply then
        local status, _, body = http.parse(reply)

        if status == 200 and m.pics:hand(m.page, w.k, body) then came = came + 1 end
      end
    end
  end

  m.loading, m.relayout, m.bar = false, true, nil
  print(("mail: %d of %d pictures came"):format(came, #f.list))

  return m == message
end

--------------------------------------------------------------------------
-- What a press does: shown at once, and asked of `maild` after.
--------------------------------------------------------------------------

local function chosen_row()
  for i, r in ipairs(shown) do
    if r.uid == chosen then return r, i end
  end

  return nil
end

local function counts_change(by)
  if current and current.box.unseen then
    current.box.unseen = math.max(0, current.box.unseen + by)
  end
end

local function set_flag(row, flag, on)
  local f = facts_of(row)

  if (f[flag] == true) == on then return end

  f[flag] = on
  fs.setattr(row.path, { [flag] = on })

  if flag == "seen" then counts_change(on and -1 or 1) end

  ask{ type = "flag", account = current.account.address, mailbox = current.box.name,
       uids = { row.uid }, flag = flag, on = on }
  print(("mail: %s %d %s"):format(flag, row.uid, on and "on" or "off"))
end

local function forget_message()
  if message and message.page then message.page:close() end

  message = nil
  band_for = nil
end

local function choose(uid)
  chosen = uid
  scroll = 0
  forget_message()

  local row = chosen_row()

  if not row then return end

  message = read_message(row)
  set_message()

  if message then
    print(("mail: showing %d, %s, from %s"):format(row.uid, message.subject, message.from))
  end

  -- Read is read the moment it is shown.
  set_flag(row, "seen", true)
end

-- Archive or Delete: gone from the list now, the next one shown, and the
-- server told.
local function take_away(kind)
  local row, i = chosen_row()

  if not row then return end

  if kind == "archive" and not (box_for(current.account, "archive")
                                or box_for(current.account, "all")) then
    said = "This account has no Archive mailbox"
    return
  end

  if not facts_of(row).seen then counts_change(-1) end

  for k, r in ipairs(rows) do
    if r.uid == row.uid then table.remove(rows, k) break end
  end

  ask{ type = kind, account = current.account.address, mailbox = current.box.name,
       uids = { row.uid } }
  print(("mail: %s %d"):format(kind, row.uid))

  filter()

  local next_row = shown[i] or shown[i - 1]

  if next_row then choose(next_row.uid) else chosen, message = nil, nil end
end

local function open_box(a, b)
  current = { account = a, box = b }
  chosen, message, query, top = nil, nil, "", 1
  read_rows()

  ask{ type = "open", account = a.address, mailbox = b.name }
  print(("mail: opened %s of %s, %d messages"):format(b.title, a.address, #rows))

  if shown[1] then choose(shown[1].uid) end
end

--------------------------------------------------------------------------
-- Add Account: a sheet over the window (`docs/mail.html`, "Adding an
-- account"). The password goes to the keyring from here and is let go;
-- `maild` signs in with it, and the account is kept only once it has.
--------------------------------------------------------------------------

local sheet = nil

local GOOGLE = { imap = { host = "imap.gmail.com", port = 993 },
                 smtp = { host = "smtp.gmail.com", port = 587 } }

local function open_sheet()
  sheet = { kind = "google", focus = 1, said = nil, waiting = nil,
            name = "", address = "", password = "",
            imap_host = "", imap_port = "993", smtp_host = "", smtp_port = "587" }
  print("mail: add account")
end

-- The fields the chosen kind asks for, in the order Tab goes through them.
local function sheet_fields()
  local list = {
    { key = "name", label = "Your name" },
    { key = "address", label = "Address" },
    { key = "password", label = sheet.kind == "google" and "App password" or "Password",
      secret = true },
  }

  if sheet.kind == "imap" then
    list[#list + 1] = { key = "imap_host", label = "Incoming" }
    list[#list + 1] = { key = "imap_port", label = "Port" }
    list[#list + 1] = { key = "smtp_host", label = "Outgoing" }
    list[#list + 1] = { key = "smtp_port", label = "Port" }
  end

  return list
end

local function guess_servers()
  local domain = sheet.address:match("@([%w%.%-]+)$")

  if not domain then return end

  if sheet.imap_host == "" then sheet.imap_host = "imap." .. domain end
  if sheet.smtp_host == "" then sheet.smtp_host = "smtp." .. domain end
end

local function servers_of()
  if sheet.kind == "google" then return GOOGLE.imap, GOOGLE.smtp end

  return { host = sheet.imap_host, port = tonumber(sheet.imap_port) or 993 },
         { host = sheet.smtp_host, port = tonumber(sheet.smtp_port) or 587 }
end

local function sign_in()
  local address = sheet.address:gsub("^%s+", ""):gsub("%s+$", "")

  if not address:match("^[^@/%s]+@[%w%.%-]+%.%a+$") then
    sheet.said = "That is not an email address"
    return
  end

  if sheet.password == "" then
    sheet.said = "The password is empty"
    return
  end

  local imap_s, smtp_s = servers_of()

  if imap_s.host == "" then
    sheet.said = "The incoming server is empty"
    return
  end

  local dir = HOME .. "/" .. address
  local ok, why = files.make_folder(dir)

  if ok then
    ok, why = fs.write(dir .. "/account", {
      address = address, name = sheet.name ~= "" and sheet.name or nil,
      kind = sheet.kind, imap = imap_s, smtp = smtp_s })
  end

  if ok then
    ok, why = fs.mail_password_keep(("imap://%s:%d"):format(imap_s.host, imap_s.port),
                                    address, sheet.password, "Mail: " .. address)
  end

  -- Let go: the keyring has it now, and this window never needs it again.
  sheet.password = ""

  if not ok then
    sheet.said = "Could not keep the account: " .. tostring(why)
    return
  end

  sheet.address, sheet.waiting, sheet.said = address, address, "Signing in\u{2026}"
  sheet.asked_at = sys.ticks()
  ask{ type = "add", account = address }
  print(("mail: signing in %s at %s"):format(address, imap_s.host))
end

-- Cancel: an account that never signed in is not kept.
local function close_sheet()
  if sheet and sheet.waiting then
    local a = sheet.waiting

    ask{ type = "remove", account = a }
    files.remove(HOME .. "/" .. a)
    print("mail: not kept " .. a)
  end

  sheet = nil
end

-- What `maild` says of the account being added.
local function sheet_heard()
  if not (sheet and sheet.waiting) then return false end

  for _, a in ipairs(status.accounts or {}) do
    if a.address == sheet.waiting then
      if a.state == "idle" or a.state == "syncing" then
        local address = sheet.waiting

        print("mail: signed in " .. address)
        sheet = nil
        read_accounts()

        local acc = account_of(address)
        local inbox = acc and (box_for(acc, "inbox") or acc.boxes[1])

        if inbox then open_box(acc, inbox) end

        return true
      elseif a.state == "error" and a.why then
        sheet.said = "The server said: " .. tostring(a.why):gsub("^the server said: ", "")
        return true
      end
    end
  end

  return false
end

--------------------------------------------------------------------------
-- Drawing.
--------------------------------------------------------------------------

local controls = {
  side = { icon = "sidebar" },
  compose = { icon = "compose" },
  reply = { icon = "reply" },
  replyall = { icon = "replyall" },
  forward = { icon = "forwardmail" },
  archive = { icon = "archive" },
  delete = { icon = "trash" },
  flag = { icon = "flag" },
  read = { icon = "mailopen" },
  fetch = { icon = "reload" },
  dots = { icon = "more" },
}

local RIGHT = { "compose", "|", "reply", "replyall", "forward", "|", "archive", "delete",
                "flag", "read", "fetch", "dots" }

local said_places = nil

local function place_controls()
  local three = (win.lights and win.lights.w) or 68
  local x = W - L.lights_in - three - L.head_edge - 26

  controls.side.x, controls.side.y = L.head_edge, pk.centre(26)

  for i = #RIGHT, 1, -1 do
    local name = RIGHT[i]

    if name == "|" then
      x = x - 10
    else
      controls[name].x, controls[name].y = x, pk.centre(26)
      x = x - 30
    end
  end

  local where = ("compose %d,%d reply %d,%d replyall %d,%d forward %d,%d side %d,%d archive %d,%d delete %d,%d flag %d,%d read %d,%d fetch %d,%d dots %d,%d list %d,%d rows %d")
    :format(controls.compose.x, controls.compose.y, controls.reply.x, controls.reply.y,
            controls.replyall.x, controls.replyall.y, controls.forward.x, controls.forward.y,
            controls.side.x, controls.side.y, controls.archive.x, controls.archive.y,
            controls.delete.x, controls.delete.y, controls.flag.x, controls.flag.y,
            controls.read.x, controls.read.y, controls.fetch.x, controls.fetch.y,
            controls.dots.x, controls.dots.y, sidebar and SIDE_W or 0, L.head + 86, ROW_H)

  if where ~= said_places then
    said_places = where
    print("mail: places " .. where)
  end
end

local function unread_total()
  local n = 0

  for _, a in ipairs(accounts) do
    local inbox = box_for(a, "inbox")

    n = n + (inbox and inbox.unseen or 0)
  end

  return n
end

-- What the Outbox is doing, for the header: "Sending 1 message", or why
-- one was not sent.
local function outbox_said()
  local sending, failed = 0, nil

  for _, o in ipairs(status.outbox or {}) do
    if o.state == "sending" then sending = sending + 1 end
    if o.state == "failed" and not failed then failed = o end
  end

  if failed then
    return ("Not sent: %s \u{2014} %s"):format(failed.subject ~= "" and failed.subject or "(no subject)",
                                            tostring(failed.why))
  end

  if sending > 0 then
    return ("Sending %d %s\u{2026}"):format(sending, sending == 1 and "message" or "messages")
  end

  return nil
end

local function draw_header(s)
  local sub = current and ("%s \u{00b7} %d unread"):format(current.box.label or current.box.title,
                                                       current.box.unseen or 0)
              or "No account yet"

  sub = outbox_said() or sub

  pk.header(s, 0, 0, W, "Mail", sub, controls.compose.x - 12, controls.side.x + 26 + 10)

  local row = chosen_row()
  local f = row and facts_of(row) or {}

  controls.side.pressed = sidebar
  controls.flag.pressed = f.flagged == true
  controls.flag.icon = f.flagged and "flagged" or "flag"
  controls.read.icon = f.seen and "mail" or "mailopen"

  for _, name in ipairs({ "archive", "delete", "flag", "read", "reply", "replyall", "forward" }) do
    controls[name].disabled = row == nil
  end

  controls.compose.disabled = #accounts == 0

  for name, b in pairs(controls) do
    if name ~= "|" then pk.iconbutton(s, b) end
  end

  -- The rules between the groups, as the drawing parts them.
  for i, name in ipairs(RIGHT) do
    if name == "|" then
      local after = controls[RIGHT[i + 1]]

      s:fill(after.x - 6, pk.centre(20), 1, 20, theme.line_soft)
    end
  end
end

local side_rows = {}

-- How far the sidebar is turned down, in pixels, and how tall all of it
-- is: Gmail's labels are more than a window holds.
local side_scroll, side_total = 0, 0
local said_side = nil

local function draw_sidebar(s)
  side_rows = {}

  if not sidebar then return end

  s:fill(0, L.head, SIDE_W, H - L.head, theme.window)
  s:fill(SIDE_W - 1, L.head, 1, H - L.head, theme.line_soft)

  local y = L.head + 10 - side_scroll

  if #accounts == 0 then
    s:text(18, y + 8, "No account yet", theme.text_dim, nil, "ui")
  end

  for _, a in ipairs(accounts) do
    local words = (a.name or a.address):upper()

    if y + 8 >= L.head then
      s:text(16, y + 8, ui.fitted(words, SIDE_W - 32, "label"), theme.text_dim, nil, "label")
    end

    y = y + 8 + gfx.height("label") + 6

    if a.state == "error" then
      if y >= L.head then
        s:text(16, y, ui.fitted(tostring(a.why), SIDE_W - 32, "ui"), theme.text_dim, nil, "ui")
      end

      y = y + gfx.height("ui") + 4
    end

    for _, b in ipairs(a.boxes) do
      local on = current and current.account.address == a.address and current.box.name == b.name
      local h = 32

      -- Only a row wholly in the column is drawn, or pressed.
      if y < L.head or y + h > H then
        y = y + h + 2
        goto next_box
      end

      if on then s:fill_round(8, y, SIDE_W - 16, h, theme.line_soft, 7) end

      local inset = 16 * math.min(3, b.depth or 0)

      pk.icon(s, USE_ICON[b.use] or "folder", 18 + inset, y + (h - 15) // 2,
              on and theme.accent or theme.text_dim)
      s:text(18 + inset + 15 + 10, y + (h - gfx.height()) // 2,
             ui.fitted(b.label or b.title or b.name, SIDE_W - 110 - inset, "ui"),
             theme.text, nil, "ui")

      if (b.unseen or 0) > 0 then
        local n = tostring(b.unseen)

        s:text(SIDE_W - 18 - gfx.measure(n), y + (h - gfx.height()) // 2, n,
               on and theme.text or theme.text_dim, nil, "ui")
      end

      side_rows[#side_rows + 1] = { x = 8, y = y, w = SIDE_W - 16, h = h, account = a, box = b }
      y = y + h + 2

      ::next_box::
    end

    y = y + 10
  end

  side_total = y + side_scroll - L.head

  -- Where the first mailboxes are, for whoever drives Mail from outside.
  local where = {}

  for i = 1, math.min(6, #side_rows) do
    local r = side_rows[i]

    where[#where + 1] = ("%s %d,%d"):format(r.box.use or "box", r.x + 40, r.y + r.h // 2)
  end

  where = table.concat(where, " ")

  if where ~= said_side then
    said_side = where
    print("mail: side " .. where)
  end
end

local search = { text = "" }
local search_focused = false
local list_rows = {}

local function when_of(epoch)
  if not epoch then return "" end

  local now = (clock.now() or {}).epoch or epoch
  local word = clock.day_word(epoch, now)

  if word == "today" then
    local t = clock.at(epoch)

    return ("%02d:%02d"):format(t.hour, t.min)
  end

  return word == "yesterday" and "Yesterday" or word
end

local function draw_list(s)
  list_rows = {}

  local x0 = sidebar and SIDE_W or 0
  local y0 = L.head

  s:fill(x0, y0, LIST_W, H - y0, theme.sunken)
  s:fill(x0 + LIST_W - 1, y0, 1, H - y0, theme.line_soft)

  -- The mailbox's name, its counts, and the search.
  s:fill(x0, y0, LIST_W - 1, 86, theme.window)
  s:fill(x0, y0 + 85, LIST_W - 1, 1, theme.line_soft)

  local title = current and (current.box.label or current.box.title) or "Mail"
  local line = said ~= "" and said
               or current and ("%d messages \u{00b7} %d unread"):format(#rows, current.box.unseen or 0)
               or "Add an account from the dots in the header"

  s:text(x0 + 12, y0 + 8, ui.fitted(title, LIST_W - 24, "title"), theme.text, nil, "title")
  s:text(x0 + 12, y0 + 8 + gfx.height("title"), ui.fitted(line, LIST_W - 24, "ui"),
         theme.text_dim, nil, "ui")

  search.x, search.y, search.w, search.h = x0 + 12, y0 + 46, LIST_W - 24, 30
  pk.field(s, search, search_focused)
  pk.icon(s, "search", search.x + 10, search.y + (search.h - 15) // 2, theme.text_dim)

  local tx, ty = search.x + 32, search.y + (search.h - gfx.height()) // 2

  s:text(tx, ty, search.text == "" and "Search" or search.text,
         search.text == "" and theme.text_dim or theme.text, nil, "ui")

  if search_focused then
    s:fill(tx + (search.text == "" and 0 or gfx.measure(search.text)), search.y + 7, 2,
           search.h - 14, theme.accent)
  end

  local y = y0 + 86
  local fits = (H - y) // ROW_H

  for i = top, math.min(#shown, top + fits) do
    local row = shown[i]
    local f = facts_of(row)
    local on = row.uid == chosen
    local unread = not f.seen

    if on then s:fill(x0, y, LIST_W - 1, ROW_H, theme.line_soft) end
    s:fill(x0, y + ROW_H - 1, LIST_W - 1, 1, theme.line_soft)

    if unread then s:disc(x0 + 12, y + 17, 4, theme.accent) end

    local when = when_of(f.date)
    local ww = gfx.measure(when)
    local marks = (f.flagged and 18 or 0) + ((f.attachments or 0) > 0 and 18 or 0)

    s:text(x0 + 24, y + 9, ui.fitted(tostring(f.from or "?"), LIST_W - 50 - ww - marks, "ui"),
           theme.text, nil, unread and "title" or "ui")
    s:text(x0 + LIST_W - 12 - ww, y + 10, when, theme.text_dim, nil, "ui")

    local mx = x0 + LIST_W - 16 - ww - marks

    if f.flagged then pk.icon(s, "flagged", mx, y + 10, 0xe5a50a) mx = mx + 18 end
    if (f.attachments or 0) > 0 then pk.icon(s, "attachment", mx, y + 10, theme.text_dim) end

    s:text(x0 + 24, y + 9 + gfx.height("title"), ui.fitted(tostring(f.subject or ""),
           LIST_W - 36, "ui"), theme.text, nil, "ui")
    s:text(x0 + 24, y + 9 + gfx.height("title") + gfx.height("ui"),
           ui.fitted(tostring(f.preview or ""), LIST_W - 36, "ui"), theme.text_dim, nil, "ui")

    list_rows[#list_rows + 1] = { x = x0, y = y, w = LIST_W - 1, h = ROW_H, uid = row.uid }
    y = y + ROW_H
  end

  if current and #shown == 0 then
    s:text(x0 + 16, y + 16, query ~= "" and "Nothing matches." or "No messages.",
           theme.text_dim, nil, "ui")
  end
end

-- A sender's initial on a disc of a colour chosen by their name.
local DISCS = { 0x0f5c4d, 0x3a5ba0, 0x8a4f7d, 0x9c6b1e, 0x4f7a28, 0x7a3b3b }

local function disc_of(name)
  local h = 0

  for i = 1, #name do h = (h * 31 + name:byte(i)) % 997 end

  return 0xff000000 | DISCS[h % #DISCS + 1]
end

local attachment_chips = {}
local paper_at = nil
local load_button = nil
local said_load = nil

local function draw_message(s)
  local x, y, w, h = pane()

  attachment_chips = {}
  paper_at = nil
  s:fill(x, y, w, h, theme.window)

  if not message then
    local words = current and #shown > 0 and "Choose a message to read it." or ""

    s:text(x + 24, y + 24, words, theme.text_dim, nil, "ui")
    return
  end

  -- The head: who, to whom, the subject, when.
  local initial = utf8.char(utf8.codepoint(message.from ~= "" and message.from or "?", 1))
  local cx = x + 20 + 20

  s:disc(cx, y + 34, 20, disc_of(message.from))
  s:text(cx - gfx.measure(initial:upper(), "title") // 2, y + 34 - gfx.height("title") // 2,
         initial:upper(), 0xffffffff, nil, "title")

  local tx = x + 20 + 40 + 12
  local when = message.date and clock.relative(message.date, (clock.now() or {}).epoch
                                                or message.date) or ""
  local ww = gfx.measure(when)

  s:text(x + w - 20 - ww, y + 16, when, theme.text_dim, nil, "ui")

  local from = message.from .. (message.address ~= message.from
                                and ("  <" .. message.address .. ">") or "")

  s:text(tx, y + 14, ui.fitted(from, w - (tx - x) - ww - 40, "title"), theme.text, nil, "title")
  s:text(tx, y + 14 + gfx.height("title"), ui.fitted("To: " .. message.to, w - (tx - x) - 20, "ui"),
         theme.text_dim, nil, "ui")
  s:text(tx, y + 14 + gfx.height("title") + gfx.height("ui") + 4,
         ui.fitted(message.subject, w - (tx - x) - 20, "title"), theme.text, nil, "title")
  s:fill(x, y + HEAD_H - 1, w, 1, theme.line_soft)

  local ay = y + HEAD_H + 8

  load_button = nil

  -- Laid out first: what it says about its pictures is the bar above it.
  if shows_html() then
    local _, _, lpw, lph = paper_box()

    lay_out_html(lpw, lph)
  end

  if message.as_html or message.bar then
    local words = message.bar or "Written in HTML, shown as its text: this Kosmos has no browser engine"

    s:fill_round(x + 20, ay, w - 40, 28, theme.sunken, 8)

    local room = w - 64

    if message.bar and not message.loading then
      load_button = { text = "Load Pictures" }
      load_button.w = pk.button_width(load_button.text)
      load_button.h = 24
      load_button.x, load_button.y = x + w - 24 - load_button.w, ay + 2
      room = room - load_button.w - 12
    end

    s:text(x + 32, ay + (28 - gfx.height()) // 2, ui.fitted(words, room, "ui"),
           theme.text_dim, nil, "ui")

    if load_button then
      pk.button(s, load_button)

      local where = ("%d,%d"):format(load_button.x + load_button.w // 2,
                                     load_button.y + load_button.h // 2)

      if where ~= said_load then
        said_load = where
        print("mail: load pictures at " .. where)
      end
    end

    ay = ay + 34
  end

  if #message.attachments > 0 then
    local cxp = x + 20

    for _, a in ipairs(message.attachments) do
      local words = ("%s  %s"):format(a.name, files.size(a.bytes * 3 // 4))
      local cw = math.min(gfx.measure(words) + 40, w - 40)

      if cxp + cw > x + w - 20 then break end

      s:fill_round(cxp, ay, cw, 32, theme.raised, 9)
      s:frame_round(cxp, ay, cw, 32, theme.line_soft, 9)
      pk.icon(s, "attachment", cxp + 10, ay + 8, theme.text_dim)
      s:text(cxp + 30, ay + (32 - gfx.height()) // 2, ui.fitted(words, cw - 38, "ui"),
             theme.text, nil, "ui")
      attachment_chips[#attachment_chips + 1] = { x = cxp, y = ay, w = cw, h = 32 }
      cxp = cxp + cw + 8
    end
  end

  -- The paper: the page, drawn into a band the paper's size when it moved.
  local px, py, pw, ph = paper_box()

  if not shows_html() then set_message() end

  clamp_scroll()

  local key = ("%d:%d:%d:%d:%s:%d"):format(message.uid, pw, ph, scroll,
                                          tostring(shows_html()), message.version or 0)

  if band_for ~= key then
    if band then
      local bw, bh = band:size()

      if bw ~= pw or bh ~= ph then band:free() band = nil end
    end

    band = band or gfx.surface{ w = pw, h = ph }

    if shows_html() then
      -- A page's own ground is white unless it says otherwise, as in a
      -- browser.
      band:fill(0, 0, pw, ph, 0xffffffff)
      message.page:ns_paint(band, pw, ph, scroll)
    else
      band:fill(0, 0, pw, ph, PAPER)

      if message.set then
        drawer:page(message.set, message.set.pages[1], band, SCALE, 0, -scroll, PAPER)
      end
    end

    band_for = key
  end

  s:blit(band, 0, 0, pw, ph, px, py)
  s:frame_round(px - 1, py - 1, pw + 2, ph + 2, theme.line_soft, 4)
  paper_at = { x = px, y = py, w = pw, h = ph }

  -- How far down, when there is more than shows.
  local total = page_px()

  -- More than a row or two more: the engine lays a page out at least as
  -- tall as its view, and a bar for one row is a bar for nothing.
  if total > ph + 4 then
    local bar = math.max(24, ph * ph // total)
    local by = py + (ph - bar) * scroll // math.max(1, total - ph)

    s:fill_round(px + pw - 6, by, 4, bar, theme.line, 2)
  end
end

local sheet_boxes = {}

local function draw_sheet(s)
  sheet_boxes = {}

  if not sheet then return end

  -- Over everything, as Passwords' confirmation is: the window's colour
  -- toward its text's.
  s:fill(0, L.head, W, H - L.head, theme.mix(theme.window, theme.text, 120))

  local cw, ch = 560, sheet.kind == "imap" and 470 or 380
  local cx, cy = (W - cw) // 2, L.head + math.max(10, (H - L.head - ch) // 2)

  s:fill_round(cx, cy, cw, ch, theme.window, 12)
  s:frame_round(cx, cy, cw, ch, theme.line_soft, 12)
  s:text(cx + 22, cy + 16, "Add Account", theme.text, nil, "title")

  -- The kinds: Google, any IMAP server, and POP3, which is later.
  local kinds = { { id = "google", t = "Google", d = "Gmail, with an app password" },
                  { id = "imap", t = "Other IMAP", d = "any server, by its address" },
                  { id = "pop3", t = "POP3", d = "later" } }
  local kw = (cw - 44 - 20) // 3

  for i, k in ipairs(kinds) do
    local kx, ky = cx + 22 + (i - 1) * (kw + 10), cy + 52
    local on = sheet.kind == k.id

    s:fill_round(kx, ky, kw, 52, theme.raised, 10)
    s:frame_round(kx, ky, kw, 52, on and theme.accent or theme.line_soft, 10)

    local ink = k.id == "pop3" and theme.text_dim or theme.text

    s:text(kx + 12, ky + 8, k.t, ink, nil, "title")
    s:text(kx + 12, ky + 8 + gfx.height("title"), ui.fitted(k.d, kw - 20, "ui"), theme.text_dim,
           nil, "ui")

    if k.id ~= "pop3" then
      sheet_boxes[#sheet_boxes + 1] = { x = kx, y = ky, w = kw, h = 52, kind = k.id }
    end
  end

  local y = cy + 52 + 52 + 16

  for i, field in ipairs(sheet_fields()) do
    local box = { x = cx + 22 + 130, y = y, w = cw - 44 - 130, h = 30 }
    local v = sheet[field.key] or ""
    local shown_text = field.secret and string.rep("\u{2022}", utf8.len(v) or #v) or v

    s:text(cx + 22, y + (30 - gfx.height()) // 2, field.label, theme.text_dim, nil, "ui")
    pk.field(s, box, sheet.focus == i)
    s:text(box.x + 10, box.y + (30 - gfx.height()) // 2, ui.fitted(shown_text, box.w - 20, "ui", true),
           theme.text, nil, "ui")

    if sheet.focus == i then
      local caret = math.min(box.w - 10, 10 + gfx.measure(shown_text))

      s:fill(box.x + caret, box.y + 7, 2, 16, theme.accent)
    end

    box.field = i
    sheet_boxes[#sheet_boxes + 1] = box
    y = y + 38

    if field.key == "password" and sheet.kind == "google" then
      s:text(cx + 22 + 130, y - 4, ui.fitted("Sixteen letters from myaccount.google.com/apppasswords",
             cw - 44 - 130, "ui"), theme.text_dim, nil, "ui")
      y = y + gfx.height() + 4
    end
  end

  if sheet.kind == "google" then
    s:fill(cx + 22, y, cw - 44, 1, theme.line_soft)
    s:text(cx + 22, y + 10, "Incoming", theme.text_dim, nil, "ui")
    s:text(cx + 22 + 130, y + 10, "imap.gmail.com \u{00b7} 993 \u{00b7} TLS", theme.text, nil, "ui")
    s:text(cx + 22, y + 10 + gfx.height() + 4, "Outgoing", theme.text_dim, nil, "ui")
    s:text(cx + 22 + 130, y + 10 + gfx.height() + 4, "smtp.gmail.com \u{00b7} 587 \u{00b7} STARTTLS",
           theme.text, nil, "ui")
  end

  if sheet.said then
    s:text(cx + 22, cy + ch - 20 - 31 - gfx.height() - 10,
           ui.fitted(sheet.said, cw - 44, "ui"), theme.text_dim, nil, "ui")
  end

  local go = { text = "Sign In", go = true }
  local cancel = { text = "Cancel" }

  go.w, cancel.w = pk.button_width(go.text), pk.button_width(cancel.text)
  go.x, go.y = cx + cw - 22 - go.w, cy + ch - 20 - 31
  cancel.x, cancel.y = go.x - 8 - cancel.w, go.y
  go.disabled = sheet.waiting ~= nil and not tostring(sheet.said):find("server said")
  pk.button(s, cancel)
  pk.button(s, go)
  sheet_boxes[#sheet_boxes + 1] = { x = go.x, y = go.y, w = go.w, h = 31, action = "go" }
  sheet_boxes[#sheet_boxes + 1] = { x = cancel.x, y = cancel.y, w = cancel.w, h = 31,
                                    action = "cancel" }

  -- Where each is, once a change, for a harness to aim at.
  local where = {}

  for _, b in ipairs(sheet_boxes) do
    local name = b.kind or b.action or ("field" .. b.field)

    where[#where + 1] = ("%s %d,%d"):format(name, b.x + b.w // 2, b.y + b.h // 2)
  end

  where = table.concat(where, " ")

  if where ~= sheet.said_where then
    sheet.said_where = where
    print("mail: sheet " .. where)
  end
end

local function draw_all()
  local s = win:surface()

  place_controls()
  draw_list(s)
  draw_message(s)
  draw_sidebar(s)
  draw_header(s)
  draw_sheet(s)

  return win:commit{ x = 0, y = 0, w = W, h = H }
end

--------------------------------------------------------------------------
-- Keys and presses.
--------------------------------------------------------------------------

local decode = keys.decoder()

local function typed_into(text, k, mods)
  if k == 8 or k == 127 then
    return text:sub(1, (utf8.offset(text, -1) or 1) - 1)
  elseif k >= 32 and (mods & keys.CTRL) == 0 then
    return text .. utf8.char(k)
  end

  return nil
end

local function sheet_key(k, mods)
  local fields = sheet_fields()

  if k == keys.ESCAPE then
    close_sheet()
  elseif k == keys.ENTER or k == 10 then
    if sheet.focus == 2 then guess_servers() end
    sign_in()
  elseif k == 9 then
    if sheet.focus == 2 then guess_servers() end
    sheet.focus = sheet.focus % #fields + 1
  else
    local field = fields[sheet.focus]
    local now = field and typed_into(sheet[field.key], k, mods)

    if not now then return false end

    sheet[field.key] = now
  end

  return true
end

local function move_choice(by)
  local _, i = chosen_row()

  i = (i or 0) + by

  if shown[i] then
    choose(shown[i].uid)

    local fits = (H - L.head - 86) // ROW_H

    if i < top then top = i end
    if i >= top + fits then top = i - fits + 1 end
  end
end

local function key(c)
  local k, mods = keys.parts(c)

  if sheet then return sheet_key(k, mods) end

  if search_focused then
    if k == keys.ESCAPE then
      search_focused = false
    elseif k == keys.ENTER or k == 10 then
      search_focused = false
    else
      local now = typed_into(search.text, k, mods)

      if not now then return false end

      search.text, query = now, now
      filter()
      print(("mail: %d shown for %q"):format(#shown, query))
    end

    return true
  end

  if c == 2 or (k == 98 and mods & keys.CTRL ~= 0) then
    sidebar = not sidebar
  elseif k == keys.DOWN then
    move_choice(1)
  elseif k == keys.UP then
    move_choice(-1)
  elseif k == keys.DELETE or k == 8 or k == 127 then
    take_away("delete")
  elseif k == keys.PAGEDOWN or k == 32 then
    scroll = scroll + 300
  elseif k == keys.PAGEUP then
    scroll = scroll - 300
  else
    return false
  end

  return true
end

local function dots_menu()
  local items = {
    { text = "Add Account\u{2026}", on_choose = function() open_sheet() end },
    { text = "Get Mail", on_choose = function() ask{ type = "sync" } end },
  }

  if message and message.html then
    items[#items + 1] = {
      text = message.as_text and "Show as HTML" or "Show as Plain Text",
      on_choose = function()
        message.as_text = not message.as_text
        scroll, band_for = 0, nil
        print("mail: shown as " .. (message.as_text and "text" or "HTML"))
      end,
    }
  end

  win:open_menu(win.origin_x + controls.dots.x, win.origin_y + L.head, items)
end

local function press_sheet(x, y)
  for _, b in ipairs(sheet_boxes) do
    if pk.inside(b, x, y) then
      if b.kind then
        sheet.kind, sheet.focus = b.kind, 1
        if b.kind == "imap" then guess_servers() end
      elseif b.field then
        if sheet.focus == 2 then guess_servers() end
        sheet.focus = b.field
      elseif b.action == "go" then
        sign_in()
      elseif b.action == "cancel" then
        close_sheet()
      end

      return
    end
  end
end

--------------------------------------------------------------------------
-- Writing: composers, each a window of its own.
--------------------------------------------------------------------------

local composers = {}

-- The account a message is written from: the mailbox shown's, or the first.
local function writing_account()
  local a = current and current.account or accounts[1]

  return a and { address = a.address, name = a.name or "" } or nil
end

local function own_addresses()
  local out = {}

  for _, a in ipairs(accounts) do out[#out + 1] = a.address end
  return out
end

-- Composers stand down and right of this window, each a little further.
local function next_place()
  local n = #composers

  return { x = (win.origin_x or 0) + 140 + n * 28, y = (win.origin_y or 0) + 60 + n * 28 }
end

local function opened(c)
  if c then composers[#composers + 1] = c end
end

local function write_new()
  local a = writing_account()

  if not a then return end

  local place = next_place()

  opened(compose.open{ account = a, x = place.x, y = place.y })
end

local function write_answer(kind)
  local row = chosen_row()
  local a = writing_account()

  if not (row and a) then return end

  local spec = compose.answer(row.path, kind, own_addresses())

  if not spec then return end

  local place = next_place()

  spec.account, spec.x, spec.y = a, place.x, place.y
  opened(compose.open(spec))

  -- Answered is a flag the server keeps, for the list's arrow.
  if kind ~= "forward" then
    ask{ type = "flag", account = current.account.address, mailbox = current.box.name,
         uids = { row.uid }, flag = "answered", on = true }
  end
end

local function write_draft(row)
  local a = writing_account()

  if a then
    opened(compose.continue(row.path, current.box.name, row.uid, a, next_place()))
  end
end

local function press(x, y)
  if sheet then
    if y < L.head then win:take_hold(x, y) else press_sheet(x, y) end
    return
  end

  local row = chosen_row()

  if pk.inside(controls.side, x, y) then
    sidebar = not sidebar
    print("mail: sidebar " .. (sidebar and "shown" or "hidden"))
  elseif pk.inside(controls.dots, x, y) then
    dots_menu()
  elseif pk.inside(controls.compose, x, y) then
    write_new()
  elseif row and pk.inside(controls.reply, x, y) then
    write_answer("reply")
  elseif row and pk.inside(controls.replyall, x, y) then
    write_answer("replyall")
  elseif row and pk.inside(controls.forward, x, y) then
    write_answer("forward")
  elseif pk.inside(controls.fetch, x, y) then
    ask{ type = "sync" }
    said = "Getting mail\u{2026}"
  elseif row and pk.inside(controls.flag, x, y) then
    set_flag(row, "flagged", not facts_of(row).flagged)
  elseif row and pk.inside(controls.read, x, y) then
    set_flag(row, "seen", not facts_of(row).seen)
  elseif row and pk.inside(controls.archive, x, y) then
    take_away("archive")
  elseif row and pk.inside(controls.delete, x, y) then
    take_away("delete")
  elseif y < L.head then
    win:take_hold(x, y)
  else
    search_focused = pk.inside(search, x, y)

    for _, r in ipairs(side_rows) do
      if pk.inside(r, x, y) then open_box(r.account, r.box) return end
    end

    for _, r in ipairs(list_rows) do
      if pk.inside(r, x, y) then
        choose(r.uid)

        -- A draft is written on, not read.
        if current and current.box.use == "drafts" then
          local row = chosen_row()

          if row then write_draft(row) end
        end

        return
      end
    end

    if load_button and pk.inside(load_button, x, y) then
      load_pictures()
      return
    end

    -- A link in an HTML message: what is under the point, in the browser.
    if shows_html() and message.page and paper_at and pk.inside(paper_at, x, y) then
      local href = message.page:ns_link_at(x - paper_at.x, y - paper_at.y + scroll)

      if href and tostring(href):match("^https?:") then
        print("mail: link " .. tostring(href))
        fs.send("/Running/wm", { type = "launch", program = "/Kosmos/Apps/browser.lua",
                                 args = tostring(href), wait = false })
      end

      return
    end

    -- A link in the message, opened in the browser.
    if message and message.set and paper_at and pk.inside(paper_at, x, y) then
      local place = pageset.hit(message.set, measure, 1, (x - paper_at.x) / SCALE,
                                (y - paper_at.y + scroll) / SCALE)

      for _, l in ipairs(place and message.links[place.para] or {}) do
        if place.at >= l.from and place.at <= l.to + 1 then
          print("mail: link " .. l.url)
          fs.send("/Running/wm", { type = "launch", program = "/Kosmos/Apps/browser.lua",
                                   args = l.url, wait = false })
          return
        end
      end
    end
  end
end

local function wheel(x, y, n)
  if sheet then return end

  if sidebar and x < SIDE_W then
    -- Three rows a notch, as everything the kit scrolls, to where the last
    -- mailbox is in view.
    side_scroll = math.max(0, math.min(side_scroll - n * ui.WHEEL_ROWS * 34,
                                       math.max(0, side_total - (H - L.head))))
    print(("mail: sidebar from %d"):format(side_scroll))
    return
  end

  local px = (sidebar and SIDE_W or 0) + LIST_W

  if x >= px then
    -- A turn down is negative, as the kit has it (`ui.WHEEL_ROWS`).
    scroll = scroll - n * 48
    clamp_scroll()
  elseif x >= (sidebar and SIDE_W or 0) then
    -- To where the last message is in view, and no further.
    local fits = (H - L.head - 86) // ROW_H

    top = math.max(1, math.min(top - n * ui.WHEEL_ROWS, math.max(1, #shown - fits + 1)))
    print(("mail: list from %d (a turn of %s at %d,%d)"):format(top, tostring(n), x, y))
  end
end

--------------------------------------------------------------------------
-- What `maild` has written since the last look.
--------------------------------------------------------------------------

local looked = 0

local function look()
  if sys.ticks() - looked < counter_hz // 2 then return false end

  looked = sys.ticks()

  local st = fs.read(STATUS)

  if type(st) ~= "table" or st.version == status.version then return false end

  status = st
  read_accounts()

  if sheet_heard() then return true end

  -- The mailbox shown, as `maild` now counts it, and its messages again if
  -- its folder changed.
  if current then
    local a = account_of(current.account.address)
    local b = a and box_of(a, current.box.name)

    if a and b then
      current = { account = a, box = b }

      local names = table.concat(fs.list(b.folder) or {}, "\0")

      if names ~= listing then
        read_rows(true)
        said = ""

        if chosen and not chosen_row() then
          chosen, message = nil, nil
        end
      end
    end
  elseif accounts[1] then
    local inbox = box_for(accounts[1], "inbox") or accounts[1].boxes[1]

    if inbox then open_box(accounts[1], inbox) end
  end

  return true
end

--------------------------------------------------------------------------
-- Start.
--------------------------------------------------------------------------

start_maild()
status = type(fs.read(STATUS)) == "table" and fs.read(STATUS) or status
read_accounts()

if accounts[1] then
  local inbox = box_for(accounts[1], "inbox") or accounts[1].boxes[1]

  if inbox then open_box(accounts[1], inbox) end
end

if #accounts == 0 or tostring(args or ""):match("%-%-add") then open_sheet() end

print(("mail: %d accounts"):format(#accounts))

if not draw_all() then return end

local dirty = false

while win.running do
  -- A composer open is typed in, so the wait is short: its keys are read
  -- here, after this window's.
  local reply = wmproto.poll(win.handle, (dirty or fetching or #composers > 0) and 1 or 12)

  if not reply then break end

  for _, ev in ipairs(reply.events or {}) do
    if win:direct_event(ev) then
      dirty = true
    elseif ev.type == "resize" then
      W, H = ev.w, ev.h
      band_for = nil
      if message then message.set, message.laid_w = nil, nil end
      dirty = true
    elseif ev.type == "close" then
      win:close()
    elseif ev.type == "theme" then
      dirty = true
    elseif ev.type == "key" then
      local a, b = decode(ev.code)

      for _, c in ipairs({ a, b }) do
        if key(c) then dirty = true end
      end
    elseif ev.type == "wheel" then
      wheel(ev.x or 0, ev.y or 0, ev.n or 0)
      dirty = true
    elseif ev.type == "mouse" and not ev.menu and ev.action == "press"
           and ev.button ~= "right" then
      press(ev.x or 0, ev.y or 0)
      dirty = true
    end
  end

  if not win.running then break end

  for i = #composers, 1, -1 do
    if not composers[i]:tend() then table.remove(composers, i) end
  end

  if look() then dirty = true end
  if step_fetch() then dirty = true end

  if dirty then
    dirty = false
    if not draw_all() then break end
  end

  -- Asked after it is shown: the press is on the screen before `maild` is.
  send_asked()
end

-- Mail closed with a message half written: kept as a draft, as closing the
-- composer would.
for _, c in ipairs(composers) do c:close() end
