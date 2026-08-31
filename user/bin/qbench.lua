-- Query time against file count. The most important number in M7.
--
--   qbench          the three sizes below
--
-- The premise of an indexed filesystem is that finding five things costs
-- the same whether there are fifty other things or eight hundred. If this
-- comes out sloped, the index is not being used and a query is a walk -
-- and then the whole reason for attributes collapses, because a walk is
-- what a directory tree already gives you for free.
--
-- So this measures the same query - one that always matches exactly five
-- nodes - against a filesystem that keeps growing around it, and judges
-- the result rather than only printing it. A benchmark nobody reads is a
-- benchmark that has already stopped working.
--
-- And it measures a *control* at every size: a `getattr`, which is one
-- round trip that touches no index and returns a fixed answer. Without it
-- this benchmark lies. The first version had no control, came out sloped,
-- and said so - but the slope was not in the index at all. A filesystem
-- with eight hundred more nodes in it is a Lua process with a much larger
-- live heap, and every allocation on the round trip pays a share of a
-- collection over that heap. The control rises for the same reason and by
-- the same amount, and what is left after subtracting it is the query.
--
-- QEMU numbers are not performance numbers. This one does not need to be:
-- it is a ratio between two measurements taken the same way in the same
-- boot, and a ratio does not care that the machine underneath is emulated.

local SIZES  = { 50, 200, 800 }
local ROUNDS = 200
local TARGET = 5
local SLOPE  = 1.6      -- how much the slowest size may cost over the fastest

local hz = fs.read("/dev/cpu").counter_hz
local made = 0

local function grow_to(total)
  while made < total do
    made = made + 1
    local ok, err = fs.setattr(("/data/bench/f%d"):format(made),
                               { kind = "filler", n = made })
    if not ok then
      print(("qbench: could not create node %d: %s"):format(made, tostring(err)))
      return false
    end
  end
  return true
end

-- The five that always match, made once and never touched again.
for i = 1, TARGET do
  local ok, err = fs.setattr(("/data/bench/target%d"):format(i),
                             { kind = "qbench-target" })
  if not ok then
    print("qbench: " .. tostring(err))
    return
  end
end

print(("%d nodes match; the filesystem grows around them")
      :format(TARGET))
print()
print(("  %-8s %-12s %-12s %-12s %s")
      :format("nodes", "getattr", "query", "difference", "matches"))

local results = {}

for _, size in ipairs(SIZES) do
  if not grow_to(size) then return end

  -- One of each first, so the measured rounds are not paying for whatever
  -- the first call after a growth spurt costs.
  fs.getattr("/data")
  fs.query("/data", { kind = "qbench-target" })

  local start = sys.ticks()

  for _ = 1, ROUNDS do
    fs.getattr("/data")
  end

  local control = (sys.ticks() - start) / ROUNDS

  start = sys.ticks()
  local found = 0

  for _ = 1, ROUNDS do
    local paths = fs.query("/data", { kind = "qbench-target" })
    found = paths and #paths or -1
  end

  local per = (sys.ticks() - start) / ROUNDS
  results[#results + 1] = { size = size, per = per - control }

  print(("  %-8d %-12.0f %-12.0f %-12.0f %d")
        :format(size + TARGET, control, per, per - control, found))

  if found ~= TARGET then
    print(("qbench: the query returned %d rather than %d - the numbers "
           .. "above are about the wrong thing"):format(found, TARGET))
    return
  end
end

local best, worst = results[1].per, results[1].per

for _, r in ipairs(results) do
  if r.per < best  then best  = r.per end
  if r.per > worst then worst = r.per end
end

print()
print(("counter runs at %d Hz; %d rounds per size"):format(hz, ROUNDS))
print("the last column is what the query costs above the round trip that "
      .. "carries it")

if best <= 0 then
  print("qbench: the query costs no more than the round trip carrying it, "
        .. "so there is nothing here to be flat or sloped about")
  return
end

local ratio = worst / best
print(("slowest / fastest: %.2f"):format(ratio))

if ratio <= SLOPE then
  print(("PASS: flat. %dx the nodes costs the query %.0f%% more, not %dx. "
         .. "The index is being used."):format(SIZES[#SIZES] // SIZES[1],
                                               (ratio - 1) * 100,
                                               SIZES[#SIZES] // SIZES[1]))
else
  print(("FAIL: sloped. The query costs %.2fx more across %dx the nodes, "
         .. "with the round trip already subtracted. That is a walk and not "
         .. "an index."):format(ratio, SIZES[#SIZES] // SIZES[1]))
end
