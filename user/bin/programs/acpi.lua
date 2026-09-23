-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- acpi: the firmware's AML - its DSDT and SSDTs - listed, and saved.
--
--   acpi          each table: its name, its size, who wrote it, and whether
--                 its bytes sum to zero
--   acpi save     each one into /home/acpi, as DSDT.aml, SSDT1.aml, ...
--
-- **Why a laptop's brightness starts here.** Nothing in Kosmos sets a
-- brightness, and the ThinkPad says how it is set in AML: an embedded
-- controller's register, or the graphics device's backlight. Kosmos runs no
-- AML and is not going to learn to for this. It hands the bytes over, and on
-- the Mac
--
--   make stick-log FILE=/home/acpi/
--   iasl -e build/stick-acpi/SSDT*.aml -d build/stick-acpi/DSDT.aml
--
-- turns them into something a person can read, with ACPICA's own tools.
--
-- One file a table, because a DSDT on its own is a few hundred kilobytes and
-- `/home` takes a megabyte at most in one write.

local DIR = "/home/acpi"

local function tables_word(n)
  return n == 1 and "1 table" or (n .. " tables")
end

local tables = {}
local ssdts = 0

for n = 1, 64 do
  local bytes = sys.firmware(n)

  if not bytes then break end

  local kind = bytes:sub(1, 4)
  local name = kind

  if kind == "SSDT" then
    ssdts = ssdts + 1
    name = "SSDT" .. ssdts
  end

  local sum = 0

  for i = 1, #bytes do
    sum = (sum + bytes:byte(i)) & 0xff
  end

  tables[#tables + 1] = {
    name = name, bytes = bytes, sums = (sum == 0),
    length = (#bytes >= 8) and string.unpack("<I4", bytes, 5) or 0,
    oem = bytes:sub(11, 16):gsub("[%z%s]+$", ""),
    oem_table = bytes:sub(17, 24):gsub("[%z%s]+$", ""),
  }
end

if #tables == 0 then
  print("acpi: no firmware tables on this machine - it describes itself "
        .. "some other way, or not at all")
  return
end

if not args:match("^%s*save%s*$") then
  for _, t in ipairs(tables) do
    print(("%-6s %8d bytes  %-6s %-8s  %s"):format(
      t.name, #t.bytes, t.oem, t.oem_table,
      (t.sums and t.length == #t.bytes) and "sums to zero"
        or "DOES NOT SUM TO ZERO"))
  end

  print(("acpi: %s - `acpi save` puts them in %s"):format(tables_word(#tables),
                                                         DIR))
  return
end

if not fs.getattr(DIR) then
  local ok, why = fs.send(DIR, { type = "mkdir" })

  if not ok then
    print("acpi: no " .. DIR .. ": " .. tostring(why))
    return
  end
end

-- Through pages rather than a message, as `diagnose` writes: a table is
-- tens or hundreds of kilobytes, and a message holds two.
for _, t in ipairs(tables) do
  local path = DIR .. "/" .. t.name .. ".aml"
  local buf = sys.memory((#t.bytes + 4095) // 4096)

  if not buf then
    print(("acpi: no memory for %d bytes"):format(#t.bytes))
    return
  end

  sys.region_write(buf, 0, t.bytes)

  local wrote, err = fs.write_from(path, buf, #t.bytes)

  sys.release(buf)

  if not wrote then
    print("acpi: " .. path .. ": " .. tostring(err))
    return
  end
end

print(("acpi: %s saved to %s - on the Mac, "
       .. "`make stick-log FILE=%s/` brings them back")
      :format(tables_word(#tables), DIR, DIR))
