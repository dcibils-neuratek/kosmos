-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- A C header's names, and the words above them: what the IDE suggests while
-- a project's C is typed (`libdoc.lua` is the same for a Lua library).
--
--   local set = cdoc.read(source)
--   -- set.list = { { name, kind, signature, doc }, ... }, sorted
--   -- set.names[name] = the same entry
--   -- set.fields["kw_surface"] = { list = ..., names = ... }, a struct's
--
-- `kind` is "function", "value" (a macro without arguments, an enum's
-- constant, a variable), "macro" (one with them), or "type" (a struct, a
-- union, an enum or a typedef).
--
-- **Read, not compiled.** A header is a run of declarations, and what an
-- editor wants of each is a name, how it is written and the comment above
-- it - which a statement at a time, braces counted and comments kept aside,
-- gives without a preprocessor. Lua's own headers write a name in
-- parentheses - `LUA_API int (lua_gettop) (lua_State *L);` - and that is
-- read as `lua_gettop`. What a header hides behind `#if` is read as well as
-- what it shows: a name offered that the build leaves out is answered by
-- TinyCC at F6, at its line.
--
-- Pure: `tools/test_cdoc.lua` reads the developer files' headers on the Mac.

local cdoc = {}

-- The words of a comment, without its marks, its stars or its first line's
-- licence line.
local function clean(text)
  text = text:gsub("^/%*+", ""):gsub("%*+/$", "")

  local out = {}

  for line in (text .. "\n"):gmatch("(.-)\n") do
    line = line:gsub("^%s*//+%s?", ""):gsub("^%s*%*+%s?", ""):gsub("%s+$", "")

    if not line:match("^Kosmos%. Copyright") then out[#out + 1] = line end
  end

  return (table.concat(out, "\n"):gsub("^%s+", ""):gsub("%s+$", ""))
end

-- One line, its comments taken out and handed to `took(text)`; what is left.
local function strip(line, state, took)
  local out, i = {}, 1

  while i <= #line do
    if state.comment then
      local e = line:find("*/", i, true)

      if not e then
        state.text[#state.text + 1] = line:sub(i)
        return table.concat(out)
      end

      state.text[#state.text + 1] = line:sub(i, e + 1)
      state.comment = false
      took(table.concat(state.text, "\n"))
      state.text = {}
      i = e + 2
    else
      local c, s = line:find("/[*/]", i)
      local q = line:find('["\']', i)

      if q and (not c or q < c) then
        -- A string or a character: kept whole, so a `/*` inside it is not
        -- a comment.
        local quote = line:sub(q, q)
        local e = q + 1

        while e <= #line and line:sub(e, e) ~= quote do
          e = e + ((line:sub(e, e) == "\\") and 2 or 1)
        end

        out[#out + 1] = line:sub(i, e)
        i = e + 1
      elseif c then
        out[#out + 1] = line:sub(i, c - 1)

        if line:sub(s, s) == "/" then
          took(line:sub(c))
          return table.concat(out)
        end

        state.comment, state.text = true, {}
        i = c
      else
        out[#out + 1] = line:sub(i)
        break
      end
    end
  end

  return table.concat(out)
end

local SKIP = { ["if"] = true, ["while"] = true, ["for"] = true, ["switch"] = true,
               ["return"] = true, ["sizeof"] = true }

-- How a declaration is written, on one line, without what only the build
-- needs to know.
local function written(text)
  return (text:gsub("%s+", " "):gsub("^%s+", ""):gsub("%s+$", "")
              :gsub("%f[%w_]LUA_API%s*", ""):gsub("%f[%w_]LUALIB_API%s*", "")
              :gsub("%f[%w_]LUAI_FUNC%s*", ""):gsub("%f[%w_]extern%s+", "")
              :gsub("%s*%(%s*", "("):gsub("%s*%)", ")")
              -- Lua's `int (lua_gettop) (lua_State *L)` as it is called.
              :gsub("%(([%a_][%w_]*)%)%(", " %1("):gsub("%s+", " "))
end

-- A struct's or union's fields, from what is between its braces.
local function fields_of(body, doc_of)
  local set = { list = {}, names = {} }

  for decl in body:gmatch("[^;]+") do
    local text = decl:gsub("%s+", " "):gsub("^%s+", "")

    if text ~= "" and not text:find("[{}]") then
      local ty = text:match("^(.-[%w_%*%s])[%a_][%w_]*%s*[%[%],:]") or
                 text:match("^(.-[%w_%*%s])[%a_][%w_]*%s*$") or ""

      for name in text:sub(#ty + 1):gmatch("%*?%s*([%a_][%w_]*)") do
        if not set.names[name] then
          local e = { name = name, kind = "value", signature = written(ty .. " " .. name),
                      doc = doc_of(name) or "" }

          set.names[name] = e
          set.list[#set.list + 1] = e
        end
      end
    end
  end

  table.sort(set.list, function(a, b) return a.name < b.name end)

  return set
end

function cdoc.read(source)
  local set = { list = {}, names = {}, fields = {} }
  local state = { comment = false, text = {} }
  local last_comment, last_line = nil, 0
  local stmt, depth, stmt_doc = {}, 0, nil
  local trailing = {}                   -- a field's comment beside it

  -- C keeps a struct's name apart from a function's - `struct kw_surface`
  -- and `kw_surface()` are both in `kosmos_window.h` - so a name may be a
  -- type and something else; `names` holds the something else.
  local seen = {}

  local function add(name, kind, signature, doc)
    if not name or SKIP[name] or name:match("^_") or name:match("_h$") then return end

    local key = (kind == "type") and ("type " .. name) or name

    if seen[key] then return end

    local e = { name = name, kind = kind, signature = signature, doc = doc or "" }

    seen[key] = true
    if kind ~= "type" or not set.names[name] then set.names[name] = e end
    set.list[#set.list + 1] = e
  end

  local function finish(text, doc)
    local flat = text:gsub("%f[%w_]__attribute__%s*%b()", ""):gsub("%s+", " "):gsub("^%s+", "")

    if flat == "" then return end

    -- A struct, a union or an enum with a body, named before the body or,
    -- by a typedef, after it.
    local head, body, tail = flat:match("^(.-){(.*)}(.-)$")

    -- A head with a `(` is a function's, whatever its parameters' types:
    -- `static void draw(struct kw_surface s)` defines no struct.
    if head and head:find("(", 1, true) then
      local name = head:match("%(%s*([%a_][%w_]*)%s*%)%s*%(") or head:match("([%a_][%w_]*)%s*%(")

      if name then add(name, "function", written(head), doc) end
      return
    end

    if head then
      local kw, tag = head:match("(%f[%w_]struct)%s+([%a_][%w_]*)")
      if not kw then kw, tag = head:match("(%f[%w_]union)%s+([%a_][%w_]*)") end
      if not kw then kw, tag = head:match("(%f[%w_]enum)%s+([%a_][%w_]*)") end
      if not kw then kw = head:match("%f[%w_](struct)") or head:match("%f[%w_](union)")
                       or head:match("%f[%w_](enum)") end

      local alias = tail:match("([%a_][%w_]*)%s*$")
      local is_typedef = head:match("%f[%w_]typedef%f[^%w_]")

      if kw == "enum" then
        for name in body:gmatch("([%a_][%w_]*)%s*[=,]?[^,]*") do
          add(name, "value", name, doc)
        end
      elseif kw then
        local fields = fields_of(body, function(name) return trailing[name] end)

        if tag then
          set.fields[tag] = fields
          add(tag, "type", kw .. " " .. tag, doc)
        end

        if is_typedef and alias then
          set.fields[alias] = fields
          add(alias, "type", "typedef " .. kw .. " " .. (tag or "") .. " " .. alias, doc)
        end
      elseif not is_typedef then
        -- A function defined here, `static inline` in a header.
        local name = head:match("%(?%s*([%a_][%w_]*)%s*%)?%s*%(")

        if name then add(name, "function", written(head), doc) end
      end

      return
    end

    -- A typedef without a body: its name is the last word, or the name a
    -- function pointer is given.
    if flat:match("^typedef%f[^%w_]") then
      local name = flat:match("%(%s*%*%s*([%a_][%w_]*)%s*%)") or flat:match("([%a_][%w_]*)%s*;?%s*$")

      add(name, "type", written(flat), doc)
      return
    end

    -- A function's declaration: the name before its `(`, or in parentheses.
    local name = flat:match("%(%s*([%a_][%w_]*)%s*%)%s*%(") or flat:match("([%a_][%w_]*)%s*%(")

    if name and not flat:match("^%s*[%(]") then
      add(name, "function", written(flat), doc)
      return
    end

    -- A variable.
    name = flat:match("([%a_][%w_]*)%s*[%[=]") or flat:match("([%a_][%w_]*)%s*$")
    add(name, "value", written(flat), doc)
  end

  local n = 0

  for line in (source .. "\n"):gmatch("(.-)\n") do
    n = n + 1

    local code = strip(line, state, function(text)
      local words = clean(text)
      local beside = line:match("^%s*[^/%s]") ~= nil

      if beside then
        -- A comment after code on its line is that line's: a field's.
        local name = line:match("([%a_][%w_]*)%s*[%[;,]")

        if name then trailing[name] = words end
      else
        last_comment, last_line = words, n
      end
    end)

    if state.comment then last_line = n end

    local directive = code:match("^%s*#%s*(%a+)")

    if directive and depth == 0 and #stmt == 0 then
      if directive == "define" then
        local name, args = code:match("^%s*#%s*define%s+([%a_][%w_]*)(%b())")

        if name then
          add(name, "macro", name .. args, (last_line >= n - 1) and last_comment or "")
        else
          name = code:match("^%s*#%s*define%s+([%a_][%w_]*)")

          if name and not name:match("_H$") and not name:match("^_") then
            local value = code:match("^%s*#%s*define%s+[%a_][%w_]*%s+(.-)%s*$") or ""

            add(name, "value", name .. (value ~= "" and (" " .. value) or ""),
                (last_line >= n - 1) and last_comment or "")
          end
        end
      end
    elseif not code:match("^%s*$") then
      if #stmt == 0 then
        stmt_doc = (last_line >= n - 1) and last_comment or ""
        trailing = {}
      end

      for i = 1, #code do
        local c = code:sub(i, i)

        stmt[#stmt + 1] = c

        if c == "{" then
          depth = depth + 1
        elseif c == "}" then
          -- A function's body ends where its brace does, below; a struct's
          -- at the `;` after it.
          depth = depth - 1
        elseif c == ";" and depth == 0 then
          finish(table.concat(stmt), stmt_doc)
          stmt = {}
        end
      end

      -- A function defined in the header has no `;` after its body.
      local text = table.concat(stmt)

      if depth == 0 and text:match("%)%s*{.*}%s*$") then
        finish(text, stmt_doc)
        stmt = {}
      end

      if #stmt > 0 then stmt[#stmt + 1] = "\n" end
    end
  end

  table.sort(set.list, function(a, b) return a.name < b.name end)

  return set
end

-- The headers a C file includes, by name: `"kosmos_window.h"` and `<stdio.h>`.
function cdoc.includes(lines)
  local out = {}

  for _, line in ipairs(lines) do
    local name = line:match('^%s*#%s*include%s*["<]([^">]+)[">]')

    if name then out[#out + 1] = name end
  end

  return out
end

-- The file's own: its functions, and what it declares at the top level.
function cdoc.own(lines)
  return cdoc.read(table.concat(lines, "\n"))
end

return cdoc
