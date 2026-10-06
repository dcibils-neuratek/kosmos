-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Shares over the network as a window shows them, checked on this computer
-- with no machine booted (`docs/sharing.md`, N6).
--
-- `user/lib/netshares.lua` decides what Tracker's Network group lists, how
-- an address typed into Connect to Server is read, what a share's status
-- line and a gone-away banner say, and what is remembered. Here smbfs's
-- answers are tables, chosen.
--
--   build/host/lua tools/test_netshares.lua

local ns = dofile("user/lib/netshares.lua")

local checks, failed = 0, 0

local function check(ok, what)
  checks = checks + 1

  if not ok then
    failed = failed + 1
    print("not ok - " .. what)
  end
end

--------------------------------------------------------------------------
-- An address, as typed.
--------------------------------------------------------------------------

local function split(text)
  local a, b, c = ns.split(text)

  return table.concat({ tostring(a), tostring(b), tostring(c) }, "|")
end

check(split("smb://192.168.1.38/Projects") == "192.168.1.38|Projects|",
      "an address and a share: " .. split("smb://192.168.1.38/Projects"))
check(split("  SMB://10.0.2.2:4450/Projects/inside/deeper/ ")
      == "10.0.2.2:4450|Projects|inside/deeper",
      "a port, the scheme in capitals, a folder within: "
      .. split("  SMB://10.0.2.2:4450/Projects/inside/deeper/ "))
check(split("diego-mac") == "diego-mac||", "a name alone: " .. split("diego-mac"))
check(split("smb://nas//") == "nas||", "slashes and nothing after: " .. split("smb://nas//"))
check(ns.split("") == nil and ns.split("smb://") == nil, "nothing is no address")
check(ns.url("10.0.2.2:4450", "Projects") == "smb://10.0.2.2:4450/Projects"
      and ns.url("nas", "") == "smb://nas", "and back")

--------------------------------------------------------------------------
-- The Network group.
--------------------------------------------------------------------------

local status = {
  { address = "10.0.2.2:4463", name = "MACPEER", state = "connected",
    share = "Music", account = "diego", dialect = "3.1.1", signing = true },
  { address = "10.0.2.2:4470", name = "", state = "asking" },
  { address = "10.0.2.2:4471", name = "", state = "answered", probe = true },
  { address = "192.168.1.20", name = "NAS", state = "away",
    dialect = "3.0", next_try_ms = 7400, in_state_ms = 61000 },
  { address = "192.168.1.21", name = "", state = "refused",
    why = "192.168.1.21 refused the account diego" },
  { address = "192.168.1.44", name = "T14", state = "connected", share = "",
    account = "diego", dialect = "3.1.1" },
}

local listing = { "MACPEER", "NAS", "T14" }
local of = { MACPEER = { "Projects", "Music" }, NAS = { "Media" }, T14 = {} }
local recent = {
  { address = "10.0.2.2:4463", share = "Projects", account = "diego", at = 1 },
  { address = "nas-salon", share = "", account = "diego", at = 2 },
}

local servers = ns.servers(status, listing, function(l) return of[l] end, recent)
local by = {}

for _, s in ipairs(servers) do by[s.address] = s end

check(#servers == 6 and by["10.0.2.2:4471"] == nil,
      "every server asked for, a probe left out, and a remembered one added: "
      .. #servers)
check(by["10.0.2.2:4463"].dot == "live" and by["10.0.2.2:4463"].path == "/Network/MACPEER"
      and #by["10.0.2.2:4463"].shares == 2
      and by["10.0.2.2:4463"].shares[2].path == "/Network/MACPEER/Music",
      "a server connected: live, its folder, and its two shares under it")
check(by["192.168.1.20"].dot == "warn" and by["192.168.1.20"].shares[1].name == "Media",
      "a server away: amber, and its share kept as last heard")
check(by["10.0.2.2:4470"].dot == "none" and by["10.0.2.2:4470"].path == nil,
      "a server being asked: no folder yet")
check(by["192.168.1.21"].lock and not by["192.168.1.21"].dot,
      "a server that refused: not signed in")
check(by["192.168.1.44"].note == "no shares" and by["192.168.1.44"].dot == "live",
      "a server signed into with no share: says so")
check(by["nas-salon"].lock and by["nas-salon"].remembered
      and servers[#servers].address == "nas-salon",
      "a server remembered and not asked since: last, and not signed in")

local rows = ns.rows(servers)

check(rows[1].heading and rows[1].action == "Connect\u{2026}"
      and rows[#rows].path == "/Network" and rows[#rows].accent,
      "the group's name with Connect, and All of the network last")
check(rows[2].id == "#server:10.0.2.2:4463" and rows[3].indent
      and rows[3].id == "/Network/MACPEER/Projects",
      "a server, then its shares indented under it")

-- Two servers that call themselves the same are told apart by smbfs.
local twins = ns.servers({
  { address = "a:1", name = "MAC", state = "connected" },
  { address = "b:2", name = "MAC", state = "connected" },
}, { "MAC (a:1)", "MAC (b:2)" }, function() return {} end, {})

check(twins[1].label == "MAC (a:1)" and twins[2].path == "/Network/MAC (b:2)",
      "two servers of one name, each by its own label: "
      .. tostring(twins[1].label))

--------------------------------------------------------------------------
-- Where a path is, and how it is said.
--------------------------------------------------------------------------

local a, b, c = ns.under("/Network/MACPEER/Projects/inside/deeper")

check(a == "MACPEER" and b == "Projects" and c == "inside/deeper",
      "a path inside a share")
check(ns.under("/Networked") == nil and ns.under("/Home/Network") == nil,
      "a path not under /Network is not")
check(select(1, ns.under("/Network")) == "", "/Network itself")
check(ns.trail("/Network/MACPEER/Projects") == "Network \u{203a} MACPEER \u{203a} Projects",
      "the trail: " .. tostring(ns.trail("/Network/MACPEER/Projects")))
check(ns.trail("/Network/MACPEER/Projects/a/b") == "\u{2026} \u{203a} a \u{203a} b",
      "deeper, the innermost two: " .. tostring(ns.trail("/Network/MACPEER/Projects/a/b")))

check(ns.status_line(status[1], 0) == "MACPEER \u{b7} SMB 3.1.1, signed \u{b7} as diego",
      "a share's status line: " .. ns.status_line(status[1], 0))
check(ns.status_line({ name = "NAS", address = "x", dialect = "3.0", signing = true,
                       sealing = true, account = "d" }, 2 * 1048576)
      == "NAS \u{b7} SMB 3.0, signed and encrypted \u{b7} as d \u{b7} 2.0 MB/s arriving",
      "sealed, and bytes arriving")

local title, words = ns.away_words(status[4], "15:41:12")

check(title == "NAS is not answering - retrying"
      and words:find("Nothing since 15:41:12.", 1, true)
      and words:find("The next try is in 8 s.", 1, true),
      "the banner: " .. title .. " / " .. words)
check(ns.away_words({ name = "", address = "x:1", trying = true }) ==
      "x:1 is not answering - trying again now",
      "trying now, said as that")

--------------------------------------------------------------------------
-- Remembered.
--------------------------------------------------------------------------

local list = {}

for i = 1, 10 do
  list = ns.remember(list, { address = "s" .. i, share = "x", at = i })
end

list = ns.remember(list, { address = "s7", share = "x", at = 11 })

check(#list == ns.RECENT_MOST and list[1].address == "s7" and list[1].at == 11
      and list[2].address == "s10",
      "newest first, one each, eight kept")
check(#ns.forget(list, "s7") == ns.RECENT_MOST - 1, "forgotten")

for _, r in ipairs(list) do
  check(r.password == nil, "a password is never among what is remembered")
end

if failed == 0 then
  print(("PASS: %d checks on shares as a window shows them (addresses as "
         .. "typed; the Network group's servers, their shares, states and "
         .. "the remembered ones; a path under /Network, its trail, a status "
         .. "line, a gone-away banner; the recent servers)."):format(checks))
else
  print(("FAIL: %d of %d checks"):format(failed, checks))
  os.exit(1)
end
