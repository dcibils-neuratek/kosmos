-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- Filter lists into WebKit's content rules, on the Mac (`user/lib/filters.lua`,
-- `docs/adblock.md` step B2). Each filter here is one of a kind EasyList,
-- EasyPrivacy or EasyList Cookie writes, and what it must become: its
-- address pattern, its options, its place in the list - and what must be
-- left out rather than guessed at.
--
-- **Every pattern written is one WebKit takes.** WebKit refuses a whole
-- list for one pattern outside its part of regular expressions, so each
-- rule this test makes is held to that part as `URLFilterParser.cpp`
-- states it. With an argument - a list as fetched - the whole list is
-- converted and held to it too, which is how the three real lists are
-- checked: `build/host/lua tools/test_filters.lua easylist.txt`.

use = use or function(path) return dofile((path:gsub("^/Kosmos/Libraries/", "user/lib/"))) end

local filters = use("/Kosmos/Libraries/filters.lua")
local json = use("/Kosmos/Libraries/json.lua")

local failures, checks = 0, 0

local function check(ok, what)
  checks = checks + 1

  if not ok then
    failures = failures + 1
    print(("not ok %d - %s"):format(checks, what))
  end
end

local function same(got, want, what)
  check(got == want, ("%s: %s, not %s"):format(what, tostring(got), tostring(want)))
end

-- WebKit's part of regular expressions (`URLFilterParser.cpp`): ASCII; no
-- disjunction, back reference, word boundary, built-in class but `.`,
-- counted repetition or lookaround; `^` only first and `$` only last.
local function webkit_takes(pattern)
  if pattern:find("[\128-\255]") then return false, "not ASCII" end

  local i, n = 1, #pattern
  local in_class = false

  while i <= n do
    local c = pattern:sub(i, i)

    if c == "\\" then
      local e = pattern:sub(i + 1, i + 1)

      if e:find("[dDwWsSbB0-9]") then return false, "\\" .. e end
      i = i + 2
    elseif in_class then
      if c == "]" then in_class = false end
      i = i + 1
    elseif c == "[" then
      in_class = true
      i = i + 1
      if pattern:sub(i, i) == "^" then i = i + 1 end
    elseif c == "|" then
      return false, "a disjunction"
    elseif c == "{" then
      return false, "a counted repetition"
    elseif c == "(" and pattern:sub(i + 1, i + 1) == "?" then
      return false, "a lookaround"
    elseif c == "^" and i ~= 1 then
      return false, "^ not first"
    elseif c == "$" and i ~= n then
      return false, "$ not last"
    else
      i = i + 1
    end
  end

  return not in_class, "an open class"
end

local KNOWN = {
  ["url-filter"] = true, ["url-filter-is-case-sensitive"] = true, ["resource-type"] = true,
  ["load-type"] = true, ["if-domain"] = true, ["unless-domain"] = true,
  ["if-top-url"] = true, ["unless-top-url"] = true,
}

local CONDITIONS = { "if-domain", "unless-domain", "if-top-url", "unless-top-url" }

-- A rule as WebKit's parser would take it, or why not.
local function valid(rule)
  local t = rule.trigger

  if type(t) ~= "table" or type(t["url-filter"]) ~= "string" then return false, "no url-filter" end

  for k in pairs(t) do
    if not KNOWN[k] then return false, "a trigger field WebKit does not know: " .. k end
  end

  local ok, why = webkit_takes(t["url-filter"])

  if not ok then return false, t["url-filter"] .. ": " .. why end

  local conditions = 0

  for _, k in ipairs(CONDITIONS) do
    if t[k] then
      conditions = conditions + 1
      if #t[k] == 0 then return false, k .. " empty" end

      for _, v in ipairs(t[k]) do
        if k:find("domain") then
          if v:find("[^a-z0-9.%-_*]") then return false, k .. " not lower case ASCII: " .. v end
        else
          local fine, because = webkit_takes(v)

          if not fine then return false, v .. ": " .. because end
        end
      end
    end
  end

  if conditions > 1 then return false, "more than one condition" end

  local a = rule.action.type

  if a == "css-display-none" then
    if type(rule.action.selector) ~= "string" then return false, "no selector" end
  elseif a ~= "block" and a ~= "ignore-previous-rules" and a ~= "block-cookies" then
    return false, "an action WebKit does not know: " .. tostring(a)
  end

  return true
end

-- One filter's rules, and their counts.
local function one(text)
  local rules, counts = filters.convert(text)

  for _, rule in ipairs(rules) do
    local ok, why = valid(rule)

    check(ok, ("%q makes a rule WebKit refuses: %s"):format(text, tostring(why)))
  end

  return rules, counts
end

local function only(text)
  local rules = one(text)

  check(#rules == 1, ("%q makes one rule, not %d"):format(text, #rules))

  return rules[1] or { trigger = {}, action = {} }
end

local function types(rule)
  return table.concat(rule.trigger["resource-type"] or {}, ",")
end

local DEFAULT = "child-document,font,image,media,raw,script,style-sheet,svg-document"

if not arg or not arg[1] then

-- Address patterns.
same(filters.pattern("||doubleclick.net^"), "^[^:]+:(//)?([^/]+\\.)?doubleclick\\.net[/:]",
     "a host and its subdomains")
same(filters.pattern("|https://ads."), "^https://ads\\.", "the start of an address")
same(filters.pattern("/banner.gif|"), "/banner\\.gif$", "the end of an address")
same(filters.pattern("/ads/*/banner"), "/ads/.*/banner", "a wildcard")
same(filters.pattern("*/adserver/*"), "/adserver/", "wildcards at the ends say nothing")
same(filters.pattern("/ad.js^"), "/ad\\.js([^-.%A-Za-z0-9_].*)?$",
     "a separator at the end: one, or the end of the address")
same(filters.pattern("&ad_type^banner"), "&ad_type[^-.%A-Za-z0-9_]banner", "a separator inside")
same(filters.pattern("||ads.example.com/track?"), "^[^:]+:(//)?([^/]+\\.)?ads\\.example\\.com/track\\?",
     "a path after a host, its ? escaped")
same(filters.pattern("-ad(1)+[x]"), "-ad\\(1\\)\\+\\[x\\]", "what regular expressions mean, escaped")
same(filters.pattern("/^https?:\\/\\/ads\\./"), "^https?:\\/\\/ads\\.", "a regular expression WebKit can take")
same(filters.pattern("/ad(s|v)\\.js/"), nil, "a disjunction refused")
same(filters.pattern("/\\d{3}x\\d{2}/"), nil, "\\d and {n} refused")
same(filters.pattern("||exämple.com^"), nil, "a name that is not ASCII refused")
same(filters.pattern(""), ".*", "an empty pattern matches every address")

-- Blocking, and what kinds of request.
local r = only("||doubleclick.net^")
same(r.action.type, "block", "a filter blocks")
same(types(r), DEFAULT, "with no kind named, every kind but the page and a pop-up")
same(r.trigger["load-type"], nil, "first and third party alike")

r = only("||tracker.example^$script,third-party")
same(types(r), "script", "$script")
same(r.trigger["load-type"][1], "third-party", "$third-party")

r = only("/ads.$~third-party")
same(r.trigger["load-type"][1], "first-party", "$~third-party")

r = only("/pixel.$image,xmlhttprequest,subdocument,stylesheet")
same(types(r), "child-document,fetch,image,style-sheet", "kinds as WebKit names them")

r = only("/promo/$~script,~image")
same(types(r), "child-document,font,media,raw,style-sheet,svg-document", "every kind but those named")

r = only("||popads.example^$popup")
same(types(r), "popup", "$popup")

r = only("||malware.example^$document")
same(types(r), "document", "a page itself blocked")

r = only("/Banner/$match-case")
same(r.trigger["url-filter-is-case-sensitive"], true, "$match-case")

r = only("/ad.js$domain=example.com|news.example.org")
same(table.concat(r.trigger["if-domain"], ","), "*example.com,*news.example.org",
     "$domain= on those sites and their subdomains")

r = only("/ad.js$domain=~example.com")
same(table.concat(r.trigger["unless-domain"], ","), "*example.com", "$domain=~ everywhere else")

local _, counts = one("/ad.js$domain=example.com|~sub.example.com")
same(counts["exclusions left out"], 1, "an exclusion beside inclusions counted: WebKit takes one condition")

r = only("/ad.js$domain=Example.COM")
same(r.trigger["if-domain"][1], "*example.com", "a domain in lower case")

r = only("/ad.js$domain=amazon.*")
same(r.trigger["if-domain"], nil, "a wildcard top-level domain is not a domain to WebKit")
same(r.trigger["if-top-url"][1], "^[^:]+://([^/:]*\\.)?amazon\\.[^/:]+[/:]", "but a page's address")

r = only("/ad.js$important")
same(r.action.type, "block", "$important blocks as any other")

-- Exceptions, after the blocking they undo.
local rules = one("||ads.example^\n@@||ads.example/ok.js")
same(#rules, 2, "a block and its exception")
same(rules[1].action.type, "block", "the block first")
same(rules[2].action.type, "ignore-previous-rules", "then the exception, which undoes what came before")

rules = one("@@||cdn.example^$script\n/ads.")
same(rules[1].action.type, "block", "an exception written first still goes after the blocks")

r = only("@@||example.org^$document")
same(r.trigger["url-filter"], ".*", "a page let through: every request on it")
same(r.trigger["if-top-url"][1], "^[^:]+:(//)?([^/]+\\.)?example\\.org[/:]", "on the page with that address")
same(r.trigger["resource-type"], nil, "of every kind")

r = only("@@$document,domain=example.org")
same(r.trigger["if-domain"][1], "*example.org", "a page let through by its domain alone")

-- Hiding.
rules = one("##.ad-slot\n##.sponsored\nexample.com##.promo")
same(#rules, 2, "hiding everywhere shares a rule; a site's has its own")
same(rules[1].action.selector, ".ad-slot, .sponsored", "selectors on every site, together")
same(rules[1].trigger["url-filter"], ".*", "on every page")
same(rules[2].action.selector, ".promo", "and the site's")
same(rules[2].trigger["if-domain"][1], "*example.com", "on that site")

rules = one("example.com,example.org##.promo\nexample.org,example.com##.box")
same(#rules, 1, "selectors on the same sites share a rule")

rules = one("##.ad\nexample.com#@#.ad")
same(#rules, 1, "a site's exception to hiding everywhere")
same(rules[1].trigger["unless-domain"][1], "*example.com", "is hiding everywhere but there")

rules = one("##.ad\n#@#.ad")
same(#rules, 0, "an exception everywhere is no hiding at all")

rules = one("a.com,b.com##.ad\na.com#@#.ad")
same(#rules, 1, "an exception on one of a selector's sites")
same(table.concat(rules[1].trigger["if-domain"], ","), "*b.com", "leaves the others")

rules = one("amazon.*,example.com##.sponsored")
same(rules[1].trigger["if-domain"], nil, "a wildcard site among others")
same(#rules[1].trigger["if-top-url"], 2, "makes every site a page's address")
same(rules[1].trigger["if-top-url"][1], "^[^:]+://([^/:]*\\.)?example\\.com[/:]", "the plain one included")

local many = {}
for i = 1, filters.GROUP + 1 do many[i] = "##.ad" .. i end
rules = one(table.concat(many, "\n"))
same(#rules, 2, "a group of selectors is kept small: one bad selector loses no more")

-- Order: hiding, its exceptions, blocking, its exceptions, pages let through.
rules = one(table.concat({
  "@@||example.org^$document", "@@||ads.example/ok", "||ads.example^",
  "@@||example.net^$elemhide", "site.example##.x", "@@||example.com^$generichide", "##.y",
}, "\n"))
local order = {}
for i, rule in ipairs(rules) do
  order[i] = rule.action.type == "css-display-none" and (rule.trigger["if-domain"] and "site" or "all")
             or rule.action.type == "block" and "block"
             or (rule.trigger["if-top-url"] and rule.trigger["if-top-url"][1]:match("example\\%.(%a+)") or "exception")
end
same(table.concat(order, " "), "all com site net block exception org",
     "hiding everywhere, $generichide, hiding on sites, $elemhide, blocking, @@, $document")

-- Left out, and counted.
_, counts = one("example.com#?#div:-abp-has(.ad)\nexample.com#$#abort-on-property-read ads\n##.a:has-text(Ad)")
same(counts["element hiding emulation or a snippet (B7)"], 3, "emulation and snippets, for B7")

_, counts = one("||example.com/ads.js$redirect=noopjs\n||example.com^$csp=script-src 'none'")
same(counts["rewrites rather than blocks ($redirect)"], 1, "$redirect=, which rewrites")
same(counts["rewrites rather than blocks ($csp)"], 1, "$csp=")

_, counts = one("/ads.$nonsense")
same(counts["an option WebKit has no way to say ($nonsense)"], 1, "an option nobody knows")

_, counts = one("! a comment\n[Adblock Plus 2.0]\n\n||ads.example^")
same(counts.comments, 2, "comments and the header")
same(counts.converted, 1, "the filter converted")

-- What a list says of itself.
local h = filters.header("[Adblock Plus 2.0]\n! Title: EasyList\n! Expires: 4 days (update frequency)\n")
same(h.title, "EasyList", "a list's title")
same(h.expires_hours, 96, "and how often it wants fetching")
same(filters.header("! Expires: 12 hours\n").expires_hours, 12, "in hours")

-- Written as WebKit reads it, and read back.
rules = one("||ads.example^\n##.ad")
local text = filters.encode(rules)
local back = json.decode(text)
same(#back, 2, "the JSON read back")
same(back[2].trigger["url-filter"], rules[2].trigger["url-filter"], "pattern for pattern")
check(select(2, text:gsub("\n", "")) == #rules + 2, "a rule a line")

end

-- A real list, given: converted whole, every rule one WebKit takes, and
-- under WebKit's ceiling.
for _, path in ipairs(arg or {}) do
  local f = assert(io.open(path, "rb"))
  local text = f:read("a")

  f:close()

  local rules, counts = filters.convert(text)
  local bad = 0

  for _, rule in ipairs(rules) do
    local ok, why = valid(rule)

    if not ok then
      bad = bad + 1
      if bad <= 5 then print(("  %s: %s"):format(path, why)) end
    end
  end

  check(bad == 0, ("%s: %d rules WebKit would refuse"):format(path, bad))
  check(#rules <= filters.MAX_RULES, ("%s: %d rules, over WebKit's %d"):format(path, #rules, filters.MAX_RULES))
  print(("%s: %d filters converted into %d rules"):format(path, counts.converted, #rules))
end

if failures > 0 then
  print(("FAIL: %d of %d checks on filters.lua"):format(failures, checks))
  os.exit(1)
end

print(("PASS: %d checks on filters.lua - Adblock Plus filters into WebKit's rules"):format(checks))
