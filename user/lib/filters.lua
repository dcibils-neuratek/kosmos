-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- Filter lists in Adblock Plus syntax, converted into WebKit's content
-- rules (`docs/adblock.md`, step B2). EasyList, EasyPrivacy and EasyList
-- Cookie are written in this syntax; WebKit's content blocker, Safari's,
-- takes JSON. eyeo's own `abp2blocklist` did the same for Adblock Plus on
-- Safari, and this follows its shape.
--
--   local filters = use("/Kosmos/Libraries/filters.lua")
--   local rules, counts = filters.convert(text)
--   fs.write(path, filters.encode(rules))
--
-- **What WebKit can be told.** A rule is a trigger - an address pattern, a
-- page's domain, a kind of request, first or third party - and an action:
-- `block`, `css-display-none` with a selector, or `ignore-previous-rules`,
-- which is how an exception works. Its address pattern is a small part of
-- JavaScript's regular expressions: no `|`, no `\d`, no `{n}`, no
-- lookahead. **One pattern it refuses fails the whole list**, so every
-- pattern written here is one of that part, and a filter that would need
-- more is counted and left out rather than guessed at.
--
-- **Order is meaning.** `ignore-previous-rules` undoes the rules before it
-- and none after, so a list is written in this order:
--   1. hiding on every site;
--   2. exceptions to that (`$generichide`);
--   3. hiding on named sites;
--   4. exceptions to all hiding (`$elemhide`);
--   5. blocking;
--   6. exceptions to blocking (`@@`);
--   7. pages let through whole (`$document`).
--
-- **Left out, and counted**: element hiding emulation and snippets
-- (`#?#`, `#$#`; step B7), filters that rewrite rather than block
-- (`$redirect=`, `$rewrite=`, `$csp=`, `$removeparam`), regular
-- expressions outside WebKit's part, and names that are not ASCII.
--
-- Pure: text in, tables out. It runs on the Mac as well, through
-- `tools/host_lua.c`, which is what `tools/test_filters.lua` does.

local filters = {}

local json = use("/Kosmos/Libraries/json.lua")

-- How many selectors share one hiding rule. WebKit drops a rule whose
-- selector it cannot parse, quietly and whole, so a group is kept small
-- enough that one bad selector costs little.
filters.GROUP = 100

-- WebKit's own ceiling on a list (`ContentExtensionParser.cpp`).
filters.MAX_RULES = 150000

-- Every kind of request a filter with no kind names: everything but the
-- page itself and a pop-up, as in Adblock Plus.
local DEFAULT_TYPES = { "child-document", "font", "image", "media", "raw", "script",
                        "style-sheet", "svg-document" }

-- Adblock Plus's kinds, as WebKit names them. `false` is understood and
-- has no WebKit kind; a filter naming only those is left out.
local TYPES = {
  script = "script", image = "image", stylesheet = "style-sheet",
  font = "font", media = "media", xmlhttprequest = "fetch",
  subdocument = "child-document", document = "document",
  websocket = "websocket", ping = "ping", popup = "popup",
  other = "other", object = "other", beacon = "ping",
  webrtc = false, webbundle = false,
}

-- Options that make a filter something other than a block or an
-- exception. A filter carrying one is left out.
local REWRITES = {
  redirect = true, ["redirect-rule"] = true, rewrite = true, csp = true,
  removeparam = true, header = true, method = true, sitekey = true,
  replace = true, permissions = true, genericblock = true,
}

-- Pseudo-classes only an extension's script understands (step B7).
local PROCEDURAL = { "%-abp%-", ":has%-text%(", ":contains%(", ":xpath%(", ":upward%(",
                     ":remove%(", ":style%(", ":matches%-css", ":min%-text%-length%(",
                     ":watch%-attr%(", ":matches%-path%(", ":matches%-attr%(",
                     ":others%(", ":if%(", ":if%-not%(", ":remove%-attr%(",
                     ":remove%-class%(", ":matches%-media%(" }

-- What separates the parts of an address for `^`: anything but a letter,
-- a digit, or `_ - . %`.
local SEPARATOR = "[^-.%A-Za-z0-9_]"

-- The start of an address on a host or any of its subdomains, for `||`.
local HOST_START = "^[^:]+:(//)?([^/]+\\.)?"

local function count(counts, what)
  counts[what] = (counts[what] or 0) + 1
end

local function ascii(s)
  return not s:find("[\128-\255]")
end

-- A domain as WebKit takes it: lower case ASCII, `*` before it for its
-- subdomains as well. `amazon.*`, on every top-level domain, WebKit's
-- domains cannot say; it is kept as `amazon.*` for `condition` below to
-- say as a page's address. nil for one it cannot take.
local function domain(name)
  name = name:lower()

  local wild = name:sub(-2) == ".*"
  local base = wild and name:sub(1, -3) or name

  if base == "" or not ascii(base) or base:find("[^a-z0-9.%-_]") then return nil end

  return wild and name or "*" .. name
end

-- A domain from `domain` as a pattern over a page's address.
local function top_url(name)
  local wild = name:sub(-2) == ".*"
  local base = (wild and name:sub(1, -3) or name:sub(2)):gsub("[%.%-]", "\\%0")

  return "^[^:]+://([^/:]*\\.)?" .. base .. (wild and "\\.[^/:]+" or "") .. "[/:]"
end

-- `if-domain` or `unless-domain` set on a trigger - or, when one of the
-- domains has a wildcard for its top-level domain, `if-top-url` or
-- `unless-top-url`, with every domain said as a page's address.
local function condition(trigger, negative, list)
  local wild = false

  for _, name in ipairs(list) do
    if name:sub(-2) == ".*" then wild = true end
  end

  if not wild then
    trigger[negative and "unless-domain" or "if-domain"] = list
    return
  end

  local urls = {}

  for i, name in ipairs(list) do urls[i] = top_url(name) end
  trigger[negative and "unless-top-url" or "if-top-url"] = urls
end

-- `a.com|~b.com` (or with commas, for hiding) into the domains included
-- and the domains excluded. nil when a name could not be taken and
-- nothing is left to say where the filter applies.
local function domains(list, sep)
  local include, exclude = {}, {}
  local lost = false

  for part in list:gmatch("[^" .. sep .. "]+") do
    local negated = part:sub(1, 1) == "~"
    local name = domain(negated and part:sub(2) or part)

    if not name then
      lost = true
    elseif negated then
      exclude[#exclude + 1] = name
    else
      include[#include + 1] = name
    end
  end

  if lost and #include == 0 and #exclude == 0 then return nil end

  return include, exclude, lost
end

-- An Adblock Plus address pattern into a WebKit `url-filter`, or nil.
function filters.pattern(text)
  if not ascii(text) then return nil, "not ASCII" end

  -- A regular expression, taken only if it is already in WebKit's part.
  if #text > 2 and text:sub(1, 1) == "/" and text:sub(-1) == "/" then
    local inner = text:sub(2, -2)

    if inner:find("|", 1, true) or inner:find("\\[dDwWsSbB]") or inner:find("{", 1, true)
       or inner:find("(?", 1, true) or inner:find("\\%d") then
      return nil, "a regular expression WebKit cannot take"
    end

    if inner:find("^", 2, true) and not inner:find("%[%^") then
      return nil, "a regular expression WebKit cannot take"
    end

    if inner:find("%$.") then return nil, "a regular expression WebKit cannot take" end

    return inner
  end

  local out = {}
  local i, n = 1, #text
  local in_host = false

  if text:sub(1, 2) == "||" then
    out[1] = HOST_START
    i, in_host = 3, true
  elseif text:sub(1, 1) == "|" then
    out[1] = "^"
    i = 2
  end

  local closed = false

  if n >= i and text:sub(n, n) == "|" then
    closed = true
    n = n - 1
  end

  -- A wildcard at either end says nothing: the pattern is not anchored.
  while i <= n and text:sub(i, i) == "*" and out[1] ~= "^" and out[1] ~= HOST_START do i = i + 1 end
  while n >= i and text:sub(n, n) == "*" and not closed do n = n - 1 end

  local literal = 0

  for j = i, n do
    local c = text:sub(j, j)

    if c == "*" then
      out[#out + 1] = ".*"
    elseif c == "^" then
      if in_host then
        -- After a host comes its port or its path, and WebKit's
        -- addresses always have a path.
        out[#out + 1] = "[/:]"
        in_host = false
      elseif j == n and not closed then
        -- A separator, or the end of the address.
        out[#out + 1] = "(" .. SEPARATOR .. ".*)?$"
      else
        out[#out + 1] = SEPARATOR
      end
    else
      if in_host and c:find("[/:?=&]") then in_host = false end

      if c:find("[%.%+%?%$%(%)%[%]%{%}\\|]") then
        out[#out + 1] = "\\" .. c
      else
        out[#out + 1] = c
      end

      literal = literal + 1
    end
  end

  if closed then out[#out + 1] = "$" end

  local filter = table.concat(out)

  if filter == "" or (literal == 0 and out[1] ~= HOST_START and out[1] ~= "^") then
    return ".*"
  end

  return filter
end

-- A request filter (`||ads.example^$third-party`) into a rule, and where
-- in the list it goes: "block", "exception", "document", "elemhide",
-- "generichide". nil and a reason when WebKit cannot say it.
local function request(line, counts)
  local exception = line:sub(1, 2) == "@@"

  if exception then line = line:sub(3) end

  local text, options = line, nil
  local regex = line:sub(1, 1) == "/" and line:find("/%$[^/]*$")

  if regex then
    text, options = line:sub(1, regex), line:sub(regex + 2)
  elseif not (line:sub(1, 1) == "/" and line:sub(-1) == "/") then
    local at = line:find("%$[^$]*$")

    if at then text, options = line:sub(1, at - 1), line:sub(at + 1) end
  end

  local trigger = {}
  local types, negated = {}, {}
  local place = exception and "exception" or "block"
  local whole = {}

  for option in (options or ""):gmatch("[^,]+") do
    local name, value = option:match("^([^=]+)=(.*)$")

    name = (name or option):lower()

    local negate = name:sub(1, 1) == "~"

    if negate then name = name:sub(2) end

    if REWRITES[name] then return nil, "rewrites rather than blocks ($" .. name .. ")" end

    if name == "third-party" or name == "3p" then
      trigger["load-type"] = { negate and "first-party" or "third-party" }
    elseif name == "first-party" or name == "1p" then
      trigger["load-type"] = { negate and "third-party" or "first-party" }
    elseif name == "match-case" then
      trigger["url-filter-is-case-sensitive"] = true
    elseif name == "domain" and value then
      local include, exclude, lost = domains(value, "|")

      if not include then return nil, "a domain that is not ASCII" end
      if lost then count(counts, "domains left out") end

      if #include > 0 then
        condition(trigger, false, include)
        if #exclude > 0 then count(counts, "exclusions left out") end
      elseif #exclude > 0 then
        condition(trigger, true, exclude)
      end
    elseif name == "important" then
      -- Not overridden by exceptions; WebKit has no such thing, and
      -- the filter blocks as any other.
    elseif exception and (name == "document" or name == "elemhide" or name == "generichide"
                          or name == "ehide" or name == "ghide") and not negate then
      whole[name == "ehide" and "elemhide" or name == "ghide" and "generichide" or name] = true
    elseif name == "all" then
      for _, t in ipairs(DEFAULT_TYPES) do types[t] = true end
      types.document, types.popup = true, true
    elseif TYPES[name] ~= nil then
      if negate then
        negated[name] = true
      elseif TYPES[name] then
        types[TYPES[name]] = true
      else
        types.none = true
      end
    else
      return nil, "an option WebKit has no way to say ($" .. name .. ")"
    end
  end

  local filter, why = filters.pattern(text)

  if not filter then return nil, why end

  -- A page let through, or its hiding: the pattern is the page's address.
  if whole.document or whole.elemhide or whole.generichide then
    -- WebKit takes one condition a rule: the page's address when the
    -- filter has one, its domains when it is only `$domain=`.
    trigger["url-filter"] = ".*"
    trigger["load-type"], trigger["url-filter-is-case-sensitive"] = nil, nil
    trigger["unless-domain"], trigger["unless-top-url"] = nil, nil

    if filter ~= ".*" then
      trigger["if-domain"] = nil
      trigger["if-top-url"] = { filter }
    elseif not (trigger["if-domain"] or trigger["if-top-url"]) then
      return nil, "an exception for every page"
    end

    local rule = { trigger = trigger, action = { type = "ignore-previous-rules" } }
    local place = whole.document and "document" or whole.elemhide and "elemhide" or "generichide"

    return rule, place
  end

  trigger["url-filter"] = filter

  local list = {}

  if next(negated) then
    for _, t in ipairs(DEFAULT_TYPES) do list[t] = true end

    for name in pairs(negated) do
      local t = TYPES[name]

      if t == "fetch" then list.raw = nil
      elseif t then list[t] = nil end
    end
  end

  for t in pairs(types) do
    if t ~= "none" then list[t] = true end
  end

  if types.none and not next(list) then return nil, "only kinds WebKit has no name for" end

  if not next(list) then
    for _, t in ipairs(DEFAULT_TYPES) do list[t] = true end
  end

  local names = {}

  for t in pairs(list) do names[#names + 1] = t end
  table.sort(names)
  trigger["resource-type"] = names

  return { trigger = trigger, action = { type = exception and "ignore-previous-rules" or "block" } }, place
end

local function procedural(selector)
  for _, p in ipairs(PROCEDURAL) do
    if selector:find(p) then return true end
  end

  return selector:sub(1, 1) == "+" or selector:sub(1, 1) == "^"
end

-- A list's text into WebKit's rules, in the order above, and counts of
-- what became of its filters: "converted", "comments", and each reason
-- one was left out.
function filters.convert(text)
  local counts = { converted = 0, comments = 0 }
  local places = { block = {}, exception = {}, document = {}, elemhide = {}, generichide = {} }

  -- Hiding, gathered first: a selector's exceptions may come after it.
  local generic = {}        -- selector -> set of domains it is not hidden on
  local generic_order = {}
  local specific = {}       -- selector -> { include set, exclude set }
  local specific_order = {}
  local generic_off = {}    -- selector -> true: an exception everywhere
  local excepted = {}       -- selector -> set of domains excepted

  for line in text:gmatch("[^\r\n]+") do
    line = line:match("^%s*(.-)%s*$")

    if line == "" or line:sub(1, 1) == "!" or line:sub(1, 1) == "[" then
      counts.comments = counts.comments + 1
    else
      local where, sep, selector = line:match("^([^#]*)(#[@?$%%]*#)(.+)$")

      if not sep then
        local rule, place = request(line, counts)

        if rule then
          local list = places[place]

          list[#list + 1] = rule
          counts.converted = counts.converted + 1
        else
          count(counts, place)
        end
      elseif sep ~= "##" and sep ~= "#@#" then
        count(counts, "element hiding emulation or a snippet (B7)")
      elseif procedural(selector) or not ascii(selector) then
        count(counts, procedural(selector) and "element hiding emulation or a snippet (B7)"
                                           or "a selector that is not ASCII")
      else
        local include, exclude = {}, {}

        if where ~= "" then
          include, exclude = domains(where, ",")
        end

        if not include then
          count(counts, "a domain that is not ASCII")
        elseif sep == "#@#" then
          if #include == 0 then
            generic_off[selector] = true
          else
            excepted[selector] = excepted[selector] or {}
            for _, d in ipairs(include) do excepted[selector][d] = true end
          end

          counts.converted = counts.converted + 1
        elseif #include == 0 then
          if not generic[selector] then
            generic[selector] = {}
            generic_order[#generic_order + 1] = selector
          end

          for _, d in ipairs(exclude) do generic[selector][d] = true end
          counts.converted = counts.converted + 1
        else
          if not specific[selector] then
            specific[selector] = { {}, {} }
            specific_order[#specific_order + 1] = selector
          end

          for _, d in ipairs(include) do specific[selector][1][d] = true end
          for _, d in ipairs(exclude) do specific[selector][2][d] = true end
          if #exclude > 0 then count(counts, "exclusions left out") end
          counts.converted = counts.converted + 1
        end
      end
    end
  end

  local function sorted(set)
    local list = {}

    for k in pairs(set) do list[#list + 1] = k end
    table.sort(list)

    return list
  end

  -- Selectors that share where they apply share rules, a group at a time.
  local function grouped(order, where_of, make)
    local groups, keys = {}, {}

    for _, selector in ipairs(order) do
      local where = where_of(selector)

      if where then
        local key = table.concat(where, ",")

        if not groups[key] then
          groups[key] = { where = where }
          keys[#keys + 1] = key
        end

        local g = groups[key]

        g[#g + 1] = selector
      end
    end

    local rules = {}

    for _, key in ipairs(keys) do
      local g = groups[key]

      for first = 1, #g, filters.GROUP do
        local part = table.move(g, first, math.min(first + filters.GROUP - 1, #g), 1, {})

        rules[#rules + 1] = make(g.where, table.concat(part, ", "))
      end
    end

    return rules
  end

  -- 1. Hiding everywhere, but where a site's exception says not.
  local hide_generic = grouped(generic_order, function(selector)
    if generic_off[selector] then return nil end

    local off = generic[selector]

    for d in pairs(excepted[selector] or {}) do off[d] = true end

    return sorted(off)
  end, function(unless, selector)
    local trigger = { ["url-filter"] = ".*" }

    if #unless > 0 then condition(trigger, true, unless) end

    return { trigger = trigger, action = { type = "css-display-none", selector = selector } }
  end)

  -- 3. Hiding on named sites, less those its exceptions name.
  local hide_specific = grouped(specific_order, function(selector)
    if generic_off[selector] then return nil end

    local on = specific[selector][1]

    for d in pairs(excepted[selector] or {}) do on[d] = nil end

    local list = sorted(on)

    return #list > 0 and list or nil
  end, function(include, selector)
    local trigger = { ["url-filter"] = ".*" }

    condition(trigger, false, include)

    return { trigger = trigger, action = { type = "css-display-none", selector = selector } }
  end)

  local rules = {}

  for _, part in ipairs({ hide_generic, places.generichide, hide_specific, places.elemhide,
                          places.block, places.exception, places.document }) do
    table.move(part, 1, #part, #rules + 1, rules)
  end

  counts.rules = #rules

  return rules, counts
end

-- Rules as the JSON WebKit compiles: one rule a line, so a list on the
-- disk can be read, and diffed, by a person.
function filters.encode(rules)
  local lines = {}

  for i, rule in ipairs(rules) do lines[i] = json.encode(rule) end

  return "[\n" .. table.concat(lines, ",\n") .. "\n]\n"
end

-- A list's own name for itself and how often it wants fetching, from
-- its header: `! Title:` and `! Expires: 4 days`.
function filters.header(text)
  local head = text:sub(1, 4096)
  local title = head:match("! Title: ([^\r\n]+)")
  local amount, unit = head:match("! Expires: (%d+) (%a+)")
  local hours

  if amount then
    hours = tonumber(amount) * (unit:find("^hour") and 1 or 24)
  end

  return { title = title, expires_hours = hours }
end

return filters
