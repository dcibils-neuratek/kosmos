-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Charts: the kinds there are, a chart's plan - what is drawn and its
-- words - and Word's part for it, through one door (`docs/office.md` O1,
-- `docs/sheets.md` S0, `docs/present.md`).
--
--   local chart = use("/Kosmos/Libraries/chart.lua")
--   chart.KINDS                        { { "column", "Column" }, ... }, in the tools' order
--   chart.name(kind)                   "Column"
--   chart.COLOURS                      the series' colours, in order
--   chart.plan(data, kind, x, y, w, h, text_w)   its art and labels (pageset's)
--   chart.part(t)                      a chart table as Word's chart part (docxwrite's)
--
-- A chart is a table whose numbers are drawn (`richtext.new_chart`,
-- `richtext.chart_data`). Write draws one on a page and writes one into a
-- DOCX, Present will put one on a slide, and Sheets will draw one from
-- cells: three applications, and this is where each of them asks.
--
-- **Where the code is, on 17 November 2026**: the kinds are here, moved
-- out of `writer.lua`. The plan and the part are still `pageset`'s and
-- `docxwrite`'s, where W7c wrote them, and this door hands each one on -
-- the files they are in were outside what O1's first change touched. Their
-- moving here is the next half of this step: `pageset` and `docxwrite` then
-- call `chart`, and nobody who calls `chart` changes a line.
--
-- `chart.COLOURS` is `pageset`'s list. `docxwrite` holds the same six as
-- hex of its own today, which is the second copy that moving the part here
-- takes away.

local chart = {}

-- The charts the Chart tool offers (W7c), and their names.
chart.KINDS = { { "column", "Column" }, { "bar", "Bar" }, { "line", "Line" }, { "pie", "Pie" } }

function chart.name(kind)
  for _, k in ipairs(chart.KINDS) do
    if k[1] == kind then return k[2] end
  end

  return kind
end

-- Read where they live, when first asked for: a window that only lists
-- the kinds loads neither the setting nor the DOCX writer for it.
local function pageset() return use("/Kosmos/Libraries/pageset.lua") end

setmetatable(chart, {
  __index = function(_, k)
    if k == "COLOURS" then return pageset().CHART_COLOURS end
  end,
})

function chart.plan(data, kind, x, y, w, h, text_w)
  return pageset().chart_plan(data, kind, x, y, w, h, text_w)
end

function chart.part(t)
  return use("/Kosmos/Libraries/docxwrite.lua").chart_part(t)
end

return chart
