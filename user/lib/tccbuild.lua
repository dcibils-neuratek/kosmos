-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Building C inside Kosmos: what the IDE's Build and `tcc` at the prompt
-- both ask (`docs/tinycc.md`, step C3).
--
--   local tccbuild = use("/Kosmos/Libraries/tccbuild.lua")
--   local r = tccbuild.build{ sources = { "/Home/Projects/Primes/primes.c" },
--                             out = "/Home/Projects/Primes/build/primes.elf" }
--   -- r.ok, r.problems = { { file, line, severity, text }, ... },
--   -- r.bytes, r.milliseconds; or nil and why, before anything was compiled
--
-- **The C Kit compiles and links; this does what only Lua can** - reach the
-- namespace. Each file TinyCC opens is asked of `reader`, read into a region
-- once a build and freed after it, so a header or the 21 MB runtime is never
-- a Lua string; the image comes back in a region, written to `out` and let
-- go. **The developer files must be this build's**: the runtime's protocol
-- stamp is held to the system's own first, and a pack from another build is
-- refused in words rather than linked into an image the system would not
-- start (`docs/tinycc.md`, Diego's decision 1).

local regions = use("/Kosmos/Libraries/regions.lua")
local files = use("/Kosmos/Libraries/files.lua")

local tccbuild = {}

tccbuild.DEVELOPER = "/Home/Developer"

function tccbuild.build(opts)
  local tcc = use("/Kosmos/Kits/tcc")
  local dev = opts.developer or tccbuild.DEVELOPER
  local held, order = {}, {}

  local function reader(path)
    local r = held[path]

    if r == nil then
      local got, size = regions.read_whole(path)

      if not got then
        held[path] = false
        return nil
      end

      r = { region = got, size = size }
      held[path] = r
      order[#order + 1] = got
    end

    if not r then return nil end
    return r.region.at, r.size
  end

  local function done()
    regions.free(table.unpack(order))
    order, held = {}, {}
  end

  -- The pack's runtime first: there, and this build's.
  local runtime = dev .. "/runtime.o"
  local at, size = reader(runtime)

  if not at then
    done()
    return nil, ("there are no developer files in %s: `make install-apps` puts them there"):format(dev)
  end

  local stamp = tcc.protostamp(at, size)

  if stamp ~= sys.protostamp then
    done()
    return nil, ("the developer files in %s are from another Kosmos (%s, this one %s): "
                 .. "`make install-apps` puts this build's there")
                :format(dev, tostring(stamp), tostring(sys.protostamp))
  end

  local started = sys.ticks()
  local r = tcc.build{
    sources = opts.sources,
    includes = { dev .. "/include" },
    prelude = dev .. "/include/kosmos_lua.h",
    link = { dev .. "/head.o", runtime, dev .. "/libgcc.a" },
    reader = reader,
  }
  local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000

  done()

  local out = { ok = r.ok, problems = r.problems,
                milliseconds = (sys.ticks() - started) * 1000 // hz }

  if r.ok and r.image then
    local folder = opts.out:match("^(.*)/[^/]*$")

    if folder and folder ~= "" and not fs.getattr(folder) then
      files.make_folder(folder)
    end

    local wrote, why = regions.write_file(opts.out, r.image, r.image.size)

    out.bytes = r.image.size
    tcc.release(r.image)

    if not wrote then
      out.ok = false
      out.problems[#out.problems + 1] = { line = 0, severity = "error",
                                          text = ("%s would not be written: %s")
                                                 :format(opts.out, tostring(why)) }
    end
  end

  return out
end

return tccbuild
