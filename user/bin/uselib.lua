-- Proof that /lib works, and the only thing that uses it until the UI kit.
local hello = use("/lib/hello.lua")
print(hello.greet(args ~= "" and args or "world"))
print("loaded twice is the same table: "
      .. tostring(use("/lib/hello.lua") == hello))
