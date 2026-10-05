-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Proof that /Kosmos/Libraries works: a library reached by `use`, and loaded once.
local hello = use("/Kosmos/Libraries/hello.lua")
print(hello.greet(args ~= "" and args or "world"))
print("loaded twice is the same table: "
      .. tostring(use("/Kosmos/Libraries/hello.lua") == hello))
