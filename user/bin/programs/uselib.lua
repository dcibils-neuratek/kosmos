-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Proof that /Kosmos/Libraries works, and the only thing that uses it until the UI kit.
local hello = use("/Kosmos/Libraries/hello.lua")
print(hello.greet(args ~= "" and args or "world"))
print("loaded twice is the same table: "
      .. tostring(use("/Kosmos/Libraries/hello.lua") == hello))
