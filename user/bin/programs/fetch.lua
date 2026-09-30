-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- One HTTP request, and whatever comes back - over TLS too.
--
--   fetch https://example.com/
--   fetch http://10.0.2.2:8000/hello
--   fetch https://10.0.2.2:8443/ --name test.local --cacert /Home/ca.der
--   fetch 10.0.2.2 8000 /                      (the form it had first)
--
-- **The smallest thing that exercises a whole connection**: open it, send
-- bytes, read bytes, notice the far end hang up. Telnet needs a person and
-- a keyboard; this needs neither, which is what makes it the thing a test
-- can drive.
--
-- HTTP/1.0 on purpose. 1.1 keeps the connection open and would need this to
-- understand `Content-Length` or chunked encoding to know when to stop;
-- 1.0's answer is that the server closes, which is exactly the event the
-- ring's `closed` flag reports. A protocol that ends by ending is the right
-- one to test a stack with.
--
-- **`https://` through the TLS Kit** (`roadmap.md`, the browser: TLS, step
-- 4): the same request, written into a TLS connection laid over the TCP
-- one, the certificate held to Mozilla's roots and the machine's clock.
-- Two options, named after curl's: `--cacert FILE`, a certificate in DER
-- to trust besides the roots - a test's own authority, or a network's; and
-- `--name NAME`, the name to hold the certificate to and send, for an
-- address given rather than a name.
--
-- The request itself is `http.lua`'s, which the browser goes through too,
-- so the two cannot disagree about what a reply is or whom to trust - the
-- image's roots and `/Home/Preferences/Authorities`.

local words, named, cacert = {}, nil, nil
local given = {}

for w in tostring(args or ""):gmatch("%S+") do given[#given + 1] = w end

do
  local i = 1

  while i <= #given do
    if given[i] == "--name" then
      named, i = given[i + 1], i + 2
    elseif given[i] == "--cacert" then
      cacert, i = given[i + 1], i + 2
    else
      words[#words + 1], i = given[i], i + 1
    end
  end
end

local http = use("/Kosmos/Libraries/http.lua")

--
-- The URL, or the form it had first: an address, a port and a path.
--
local url = words[1] or ""

if not url:match("^%a[%w+.%-]*://") and (words[2] or words[3]) then
  url = ("http://%s:%s%s"):format(url, words[2] or "80", words[3] or "/")
end

if url == "" then
  print("fetch: fetch https://name/path, or fetch <address> [port] [path]")
  return
end

local anchors = {}

if cacert then
  local der = fs.read(cacert)

  if type(der) ~= "string" or der == "" then
    print("fetch: --cacert " .. cacert .. ": no such file")
    return
  end

  anchors[1] = der
end

local reply, why, how = http.get(url, { name = named, anchors = anchors })

if not reply then
  print("fetch: " .. tostring(why))
  return
end

--
-- A connection that ended in an error after bytes came - a TLS one cut off
-- without its close, say - still printed, with why.
--
if how.ended then
  print("fetch: " .. how.ended)
end

print(("%d bytes"):format(#reply))
print(reply)
