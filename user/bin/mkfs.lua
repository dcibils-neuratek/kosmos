-- Formats the disk.
--
--   mkfs            says what is there and what it would do
--   mkfs --yes      does it
--
-- Two steps deliberately. This erases every file on the machine, and a
-- program that does that on a bare name is a program that does it by
-- accident.
--
-- The formatting itself happens in the disk server, not here: this program
-- cannot reach a sector. It writes to `/disk/format`, and the confirmation
-- travels with the request, so the check lives at the boundary rather than
-- in the habits of one caller.

local sb, err = fs.read("/disk/super")

if not sb then
  print("mkfs: " .. tostring(err))
  return
end

print(("disk: %d sectors, %d MB"):format(sb.sectors,
                                         sb.bytes // (1024 * 1024)))

if sb.formatted then
  print(("There is already a filesystem here: %d blocks of %d bytes.")
        :format(sb.blocks, sb.block_size))
else
  print("There is no filesystem here yet.")
end

if args:match("^%s*(%S*)") ~= "--yes" then
  print("")
  print("Nothing written. `mkfs --yes` formats, and erases everything.")
  return
end

local made, why = fs.write("/disk/format", "yes, erase it")

if not made then
  print("mkfs: " .. tostring(why))
  return
end

print("")
print("Formatted. `diskinfo` says what is there.")
