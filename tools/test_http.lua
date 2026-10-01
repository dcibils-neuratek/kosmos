-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- `user/lib/http.lua` on the Mac, what of it needs no machine: a refresh's
-- words, as a `<meta http-equiv="refresh">` and the `Refresh` header say
-- them (`roadmap.md` 6zz, meta refresh) - DuckDuckGo's, the forms HTML
-- allows, and what is not one.

local http = assert(loadfile("user/lib/http.lua"))()

local failures, checks = 0, 0

local function check(ok, what)
  checks = checks + 1

  if not ok then
    failures = failures + 1
    print(("not ok %d - %s"):format(checks, what))
  end
end

local function said(content)
  local seconds, address = http.refresh(content)

  return tostring(seconds) .. " " .. tostring(address)
end

check(said('0; url="https://html.duckduckgo.com/html"')
      == "0 https://html.duckduckgo.com/html", "DuckDuckGo's, its address in quotes")
check(said("5; URL=next.html") == "5 next.html", "URL in capitals, no quotes")
check(said("3,url='a b.html'") == "3 a b.html", "a comma, and single quotes")
check(said("10; next.html") == "10 next.html", "url= left out")
check(said("  7  ") == "7 nil", "the seconds alone: the page itself again")
check(said("2.5; url=x") == "2 x", "a fraction's whole seconds")
check(said(".5; url=x") == "0 x", "a fraction alone")
check(said("1;url = spaced.html ") == "1 spaced.html", "spaces round = and after")
check(said("0; url=") == "0 nil", "url= with nothing after it: the page itself")
check(said("soon; url=x") == "nil nil", "words that are not a number are not a refresh")
check(said("5x; url=x") == "nil nil", "nor a number run into a word")
check(said("") == "nil nil" and said(nil) == "nil nil", "nor nothing")

if failures == 0 then
  print(("PASS: %d checks on http.lua's refresh, on this machine."):format(checks))
  os.exit(0)
end

print(("FAIL: %d of %d checks on http.lua's refresh."):format(failures, checks))
os.exit(1)
