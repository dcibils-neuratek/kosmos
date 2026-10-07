-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- C as the IDE colours it, on this machine (`user/lib/clex.lua`): which
-- bytes of a line are a keyword, a number, a string, a comment, a call or a
-- name a C app is handed, and what a comment, a string or a directive
-- carries to the next line.
--
--   build/host/lua tools/test_clex.lua

local lex = assert(loadfile(arg and arg[1] or "user/lib/clex.lua"))()

local passed, failed = 0, 0

local function check(condition, what)
  if condition then
    passed = passed + 1
  else
    failed = failed + 1
    print("  FAIL: " .. what)
  end
end

local function spans(text, state)
  local got, out = lex.line(text, state)
  local words = {}

  for _, s in ipairs(got) do
    words[#words + 1] = s[3] .. ":" .. text:sub(s[1], s[2])
  end

  return table.concat(words, " "), out
end

local function is(text, want, what, state)
  local got = spans(text, state)

  check(got == want, ("%s: %q became %q, not %q"):format(what, text, got, want))
end

-- The templates' own lines.
is('#include "kosmos_window.h"', 'keyword:#include string:"kosmos_window.h"',
   "an include of a header in quotes")
is('#include <stdint.h>', 'keyword:#include string:<stdint.h>',
   "an include of a header in brackets")
is('static int l_fill(lua_State *L)',
   'keyword:static keyword:int call:l_fill library:lua_State',
   "a function's head: keywords, its name a call, Lua's type handed")
is('    uint32_t *px = kosmos_surface_pixels(L, 1, &w, &h, &pitch);',
   'library:uint32_t library:kosmos_surface_pixels number:1',
   "a kit's function is the library's colour")
is('            double cr = -2.3 + 3.2 * x / w;',
   'keyword:double number:2.3 number:3.2', "numbers with a point")
is('    return 0xff000000u | (r << 16);',
   'keyword:return number:0xff000000u number:16', "a hex number with its suffix")
is("    if (e.type == KW_KEY && e.key == ' ') {",
   "keyword:if library:KW_KEY string:' '", "a character, and a kit's constant")
is('    printf("plasma: %u frames\\n", frames); // said once',
   'call:printf string:"plasma: %u frames\\n" comment:// said once',
   "a string with an escape, and a comment to the end of the line")
is('int a; /* one */ int b;', 'keyword:int comment:/* one */ keyword:int',
   "a comment closed on its own line")

-- What carries on.
do
  local _, after = spans("/* a comment")
  check(after == "c", "an open comment carries on")
  is("still a comment */ int x;", "comment:still a comment */ keyword:int",
     "a comment closed on the next line", "c")
  _, after = spans("#define TWICE(x) \\")
  check(after == "p", "a directive ending in a backslash carries on")
  _, after = spans("    ((x) * 2)", "p")
  check(after == nil, "and stops on the line that does not")
  _, after = spans('char *s = "half \\')
  check(after == 'q"', "a string ending in a backslash carries on")
  is('the rest";', 'string:the rest"', "and ends on the next line", 'q"')
  _, after = spans("#include <stdio.h>")
  check(after == nil, "an ordinary directive carries nothing")
end

if failed > 0 then
  print(("FAIL: %d of %d checks on C as the editor colours it"):format(failed, passed + failed))
  os.exit(1)
end

print(("PASS: %d checks on C as the editor colours it (keywords, numbers, strings, "
       .. "characters, comments of both kinds, directives and includes, calls, the "
       .. "names a C app is handed, and what carries to the next line)"):format(passed))
