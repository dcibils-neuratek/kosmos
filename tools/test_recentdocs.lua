-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The recent documents library, held on the Mac (`user/lib/recentdocs.lua`).
--
--   build/host/lua tools/test_recentdocs.lua

local recentdocs = dofile("user/lib/recentdocs.lua")

local checks, failed = 0, 0

local function ok(what, cond)
  checks = checks + 1
  if not cond then
    failed = failed + 1
    print("  FAIL: " .. what)
  end
end

-- The Text Editor's list before 14 October: plain paths, kept.
local old = recentdocs.parse({ "/Home/a.md", "/Home/b.md", "/Home/a.md", "nowhere", 42 })
ok("plain paths read as entries", #old == 2 and old[1].path == "/Home/a.md" and old[2].path == "/Home/b.md")

-- Added: at the top, once; what the entry held kept under the new fields.
local list = recentdocs.add({}, "/Home/x.pdf", { page = 3, fit = "width" })
list = recentdocs.add(list, "/Home/y.pdf", { page = 1 })
list = recentdocs.add(list, "/Home/x.pdf", { page = 9 })
ok("the newest at the top", list[1].path == "/Home/x.pdf" and list[2].path == "/Home/y.pdf")
ok("each once", #list == 2)
ok("new fields over the old, the rest kept", list[1].page == 9 and list[1].fit == "width")
ok("found by path", recentdocs.find(list, "/Home/y.pdf").page == 1)
ok("not found is nil", recentdocs.find(list, "/Home/z.pdf") == nil)

-- Bounded: the oldest falls off, and what it held with it.
local many = {}
for i = 1, 12 do many = recentdocs.add(many, "/Home/" .. i .. ".pdf", { page = i }) end
ok("no more than MOST", #many == recentdocs.MOST)
ok("the oldest gone", recentdocs.find(many, "/Home/1.pdf") == nil
                      and recentdocs.find(many, "/Home/12.pdf").page == 12)

-- What is kept reads back the same.
local again = recentdocs.parse(recentdocs.format(list))
ok("kept and read back", #again == 2 and again[1].page == 9 and again[1].fit == "width")

-- The given list is not changed.
local before = recentdocs.add({}, "/Home/p.pdf", { page = 2 })
local _ = recentdocs.add(before, "/Home/p.pdf", { page = 5 })
ok("add makes a new list", before[1].page == 2)

-- The menu: a name and its folder, or one item saying nothing yet.
local opened = nil
local items = recentdocs.items(list, function(p) opened = p end)
ok("an item a document, named and placed", #items == 2 and items[1].text == "x.pdf"
                                            and items[1].hint == "/Home")
items[2].on_choose()
ok("choosing opens it", opened == "/Home/y.pdf")
local none = recentdocs.items({}, function() end)
ok("nothing yet, said", #none == 1 and none[1].disabled == true)

if failed > 0 then
  print(("FAIL: %d of %d checks on recent documents"):format(failed, checks))
  os.exit(1)
end

print(("PASS: %d checks on recent documents (paths from before kept, once each and newest "
       .. "first, where each was left carried and bounded by the list, the menu)"):format(checks))
