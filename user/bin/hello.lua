-- The smallest program there is, and a demonstration of what one gets.
--
-- It runs in its own address space, with its own lua_State and its own
-- heap, and can reach exactly what the shell chose to hand it. Crashing
-- here cannot take the shell down: try `error("boom")` at the end.

print("Hello from a process of my own.")
print("")
print("  what I was given:  " .. table.concat(fs.mounts(), "  "))
print("  arguments:         " .. (args ~= "" and args or "(none)"))
print("")
print("I cannot reach anything that is not on that list. There is no")
print("global filesystem to walk and no name to guess: a capability is an")
print("index into my own table, and I only have the ones I was handed.")
