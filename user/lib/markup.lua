-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Text made safe to put inside markup, HTML or XML.
--
--   local markup = use("/Kosmos/Libraries/markup.lua")
--   markup.escape('a < b & "c"')     -->  a &lt; b &amp; &quot;c&quot;
--
-- The four characters that mean something in an element's text or a
-- double-quoted attribute are escaped - `&`, `<`, `>` and `"` - and the
-- bytes below a space that XML 1.0 does not allow are dropped: tab, line
-- feed and carriage return stay, and the rest of the C0 controls go. HTML
-- does not need the second half and is not harmed by it, so one escaper
-- serves both.
--
-- **There were two**, the same table in each: Kosmos Write's DOCX writer
-- (`docxwrite.lua`) and the browser's own pages - a refused certificate,
-- a new tab (`browser.lua`). Found on 4 October and made one, under the
-- premise in `CLAUDE.md`: a second copy of anything is a defect. A third
-- caller uses this rather than a table of its own.

local markup = {}

local ESCAPE = { ["&"] = "&amp;", ["<"] = "&lt;", [">"] = "&gt;", ['"'] = "&quot;" }

function markup.escape(s)
  return (tostring(s):gsub("[%z\1-\8\11\12\14-\31]", ""):gsub('[&<>"]', ESCAPE))
end

return markup
