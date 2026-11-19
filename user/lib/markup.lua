-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Text made safe to put inside markup, HTML or XML - and HTML made text.
--
--   local markup = use("/Kosmos/Libraries/markup.lua")
--   markup.escape('a < b & "c"')     -->  a &lt; b &amp; &quot;c&quot;
--   markup.text("<p>a &amp; b</p>")  -->  a & b, as lines
--
-- The four characters that mean something in an element's text or a
-- double-quoted attribute are escaped - `&`, `<`, `>` and `"` - and the
-- bytes below a space that XML 1.0 does not allow are dropped: tab, line
-- feed and carriage return stay, and the rest of the C0 controls go. HTML
-- does not need the second half and is not harmed by it, so one escaper
-- serves both.
--
-- **There were two**, the same table in each: Kosmos Write's DOCX writer
-- (`docxwrite.lua`) and the old browser's own pages. Found on 4 October and
-- made one, under the premise in `CLAUDE.md`: a second copy of anything is
-- a defect. The browser has gone and the DOCX writer is its one caller
-- today; the next one uses this rather than a table of its own.

local markup = {}

local ESCAPE = { ["&"] = "&amp;", ["<"] = "&lt;", [">"] = "&gt;", ['"'] = "&quot;" }

function markup.escape(s)
  return (tostring(s):gsub("[%z\1-\8\11\12\14-\31]", ""):gsub('[&<>"]', ESCAPE))
end

--
-- **HTML as text**: styles and scripts left out, a paragraph's, a line
-- break's, a block's and a table row's ends as lines, every other tag
-- taken out, the few entities a message uses undone, and runs of spaces
-- and of blank lines made one. For a message quoted in a reply, a mail
-- shown before its HTML is drawn, and a page an automation looks into -
-- which had a copy each and disagreed about scripts and rows (`cleanup.md`
-- finding 190). Not a parser: `mailweb.md` draws HTML with WebKit.
--
function markup.text(html)
  local t = tostring(html):gsub("<[Ss][Tt][Yy][Ll][Ee].-</[Ss][Tt][Yy][Ll][Ee]>", "")
                :gsub("<[Ss][Cc][Rr][Ii][Pp][Tt].-</[Ss][Cc][Rr][Ii][Pp][Tt]>", "")
                :gsub("<[Bb][Rr]%s*/?>", "\n")
                :gsub("</?[Pp][^>]*>", "\n\n")
                :gsub("</?[Dd][Ii][Vv][^>]*>", "\n")
                :gsub("</?[Tt][Rr][^>]*>", "\n")
                :gsub("<[^>]*>", "")
                :gsub("&nbsp;", " "):gsub("&lt;", "<"):gsub("&gt;", ">")
                :gsub("&quot;", '"'):gsub("&#39;", "'"):gsub("&amp;", "&")
                :gsub("[ \t]+", " "):gsub("\n[ \t]+", "\n"):gsub("\n\n\n+", "\n\n")

  return t
end

return markup
