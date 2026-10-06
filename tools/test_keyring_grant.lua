-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Which files a launcher hands the keyring's `manage` door to
-- (`docs/keyring.md`, K4; `keyring_grant` in `user/init/init.lua`).
--
-- **Tested here because nothing on the machine can reach it yet.** Only the
-- image's server says what a file `needs` (`binfs.c`), so a file in `/Home`
-- declares nothing at all and is refused before this rule is asked -
-- `run_keyring.py`'s forgery passes with the rule switched off. The rule is
-- for the day a file on the disk can declare (`roadmap.md`, per-launcher
-- permissions), and until then it is held here: the function is taken out of
-- `init.lua` by its text, as it is, and asked about paths.
--
--   lua tools/test_keyring_grant.lua user/init/init.lua

local source = assert(io.open(arg[1] or "user/init/init.lua")):read("a")
local body = source:match("\n(local function keyring_grant%(path%).-\nend)\n")
assert(body, "keyring_grant is not in init.lua as one function")

local keyring_grant = assert(load(body .. "\nreturn keyring_grant"))()
local checks, fails = 0, {}

local function expect(path, want)
  checks = checks + 1
  if keyring_grant(path) ~= want then
    fails[#fails + 1] = ("%q: %s, not %s"):format(tostring(path),
                                                 tostring(not want), tostring(want))
  end
end

-- The image's, granted.
expect("/Kosmos/Apps/passwords.lua", true)
expect("/Kosmos/Programs/keyring.lua", true)
expect("/Kosmos/Apps/browser/browser.lua", true)

-- Anything else, refused - and every way of spelling the way out.
expect("/Home/keyring.lua", false)
expect("/Home/Apps/Passwords/passwords.lua", false)
expect("/Kosmos/Apps/../../Home/keyring.lua", false)
expect("/Kosmos/Apps/./../x.lua", false)
expect("/Kosmos/Apps/..", false)
expect("/Kosmos/Apps//x.lua", false)
expect("/Kosmos/Apps/", false)
expect("/Kosmos/Apps", false)
expect("/Kosmos/AppsX/x.lua", false)
expect("/Kosmos/Libraries/x.lua", false)
expect("/Temporary/Kosmos/Apps/x.lua", false)
expect("Kosmos/Apps/x.lua", false)
expect("/kosmos/apps/x.lua", false)
expect(nil, false)
expect(42, false)

if #fails > 0 then
  print(("FAIL: %d of %d checks on who is handed the keyring:\n  %s")
        :format(#fails, checks, table.concat(fails, "\n  ")))
  os.exit(1)
end

print(("PASS: %d checks on who is handed the keyring (the image's apps and "
       .. "programs; nothing in /Home, nothing through . or .. or //, nothing "
       .. "that only looks like /Kosmos/Apps)"):format(checks))
