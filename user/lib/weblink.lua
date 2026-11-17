-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- A web page opened from any program: the one door to the browser.
--
-- Mail's links, Cafesa3D's and the IDE's tutorials and the menu's
-- Documentation pages all open here, so they all open the same way: in a
-- new tab of the JetSki that is open, or in a new JetSki when none is
-- (`docs/mailweb.md`, decided by Diego on 15 November 2026). It replaced
-- five launches of the old browser, each written where it was needed
-- (`roadmap.md`, *The old browser removed*, 17 November).
--
--   local weblink = use("/Kosmos/Libraries/weblink.lua")
--   weblink.open("https://en.wikipedia.org/wiki/BeOS")
--   weblink.open("/Kosmos/Documentation/cheatsheet.html")
--
-- True when the page was handed over, or false and why.

local weblink = {}

-- The open JetSki's name, as it registers its window.
weblink.RUNNING = "/Running/JetSki/open"

function weblink.open(address)
  address = tostring(address or "")
  if address == "" then return false, "no address" end

  -- A JetSki that is open takes it in a tab. A write to a name nobody holds
  -- is refused, and that is the only answer that starts one.
  if fs.write(weblink.RUNNING, address) then return true end

  local ok, why = fs.send("/Running/wm", { type = "launch", program = "jetski",
                                           args = address, wait = false })
  if not ok then return false, why end
  return true
end

return weblink
