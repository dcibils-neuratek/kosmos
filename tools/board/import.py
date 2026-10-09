#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The live roadmap list, onto the Kosmos Board, once.

    import.py ITEMS_DIR OUT.sql

ITEMS_DIR holds one JSON file an item, as the live list's database gave
them (9 October 2026, 494 items); OUT.sql is what `wrangler d1 execute
--file` runs. Nothing of the roadmap's content is in this repository: it
is read from where it was exported, under build/, and goes to the board's
own database.

A status becomes a column - wanted and noted are Ideas, held is Parked, and
one Diego dropped is a removed card, kept to bring back. Next keeps the
order Diego set on 8 October: Mail, then POP3, then the Astra split. The
decisions are the ones the roadmap records, with his words, and the two
still open.
"""

import json
import os
import sys
from datetime import datetime, timezone

COLUMN = {"wanted": "ideas", "noted": "ideas", "agreed": "agreed", "next": "next",
          "building": "building", "done": "done", "held": "parked", "dropped": "done"}

# Next, in Diego's order: "do chrome step 3 first, then mail, then the split",
# and POP3 after Mail's last step.
NEXT = ["mail-m6-writing", "mail-m7-attachments", "mail-m8-search-accounts", "mail-pop3",
        "astra-d1-decisions", "astra-split-d2"]

DECIDED = [
    ("2026-10-09", None, "Mail is not built as an IDE project, nor shipped as an IDE example (M9 dropped)",
     "Dropped on the live list", "dropped on the live roadmap list"),
    ("2026-10-09", None, "A live list of the whole roadmap, then a board to run it from, on Cloudflare, its code in the Kosmos repository",
     "As drawn", "Let's keep a live list of items we can see and maintain as our roadmap"),
    ("2026-10-08", "mail-pop3", "How Gmail connects to Kosmos Mail", "IMAP first, POP3 later", "IMAP first, POP3 later"),
    ("2026-10-08", "mail-m6-writing", "Kosmos Mail as designed", "Build it as recommended",
     "as recommended, go ahead and build it"),
    ("2026-10-08", "astra-split-d2", "The order of the work", "Chrome step 3, then Mail, then the split",
     "do chrome step 3 first, then mail, then the split"),
    ("2026-10-08", None, "Which suites a run includes", "x86 only, until said otherwise",
     "from now on lets do tests on x86 only and leave arm for a later period"),
    ("2026-10-07", "optimisation-phase", "When to optimise", "Applications first, then an optimisation phase",
     "I want to keep building apps that makes the os useful and productive before entering an optimization phase"),
    ("2026-10-07", "astra-name", "The desktop's name", "Astra", "lets call our entire desktop Astra"),
]

OPEN = [
    ("astra-d1-decisions", "Who draws an ordinary window once the window server is split out?",
     ["Every window draws itself (recommended)", "The server draws plain ones"]),
    ("astra-d1-decisions", "What is the window server called?", []),
]


def q(v):
    if v is None:
        return "NULL"
    if isinstance(v, (int, float)):
        return repr(v)
    return "'" + str(v).replace("'", "''") + "'"


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)

    src, out = sys.argv[1], sys.argv[2]
    at = datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")
    items = []

    for name in sorted(os.listdir(src)):
        if name.endswith(".json"):
            d = json.load(open(os.path.join(src, name)))
            d["id"] = name[:-5]
            items.append(d)

    lines = ["DELETE FROM cards;", "DELETE FROM notes;", "DELETE FROM decisions;", "DELETE FROM changes;"]
    by_col = {}

    for d in items:
        by_col.setdefault(COLUMN.get(d.get("status"), "ideas"), []).append(d)

    for col, list_ in by_col.items():
        if col == "next":
            list_.sort(key=lambda d: (NEXT.index(d["id"]) if d["id"] in NEXT else 99, d.get("asked", "")))
        elif col == "done":
            list_.sort(key=lambda d: d.get("asked", ""), reverse=True)
        else:
            list_.sort(key=lambda d: (d.get("asked", "") or "9999", d.get("title", "")))

        for rank, d in enumerate(list_, 1):
            removed = at if d.get("status") == "dropped" else None
            lines.append(
                "INSERT INTO cards (id, title, col, rank, area, ref, detail, quote, asked, in_roadmap, created, updated, updated_by, removed) "
                "VALUES (%s);" % ", ".join(q(v) for v in (
                    d["id"], d.get("title", d["id"]), col, rank, d.get("area", ""), d.get("ref", ""), d.get("detail", ""),
                    d.get("quote", ""), d.get("asked", ""), 1 if d.get("in_roadmap", True) else 0, at,
                    d.get("updated") or at, "Claude" if not d.get("updated") else "Diego", removed)))

            if d.get("note"):
                lines.append("INSERT INTO notes (card, text, author, at) VALUES (%s, %s, 'Claude', %s);"
                             % (q(d["id"]), q(d["note"]), q(at)))

    for day, card, question, answer, words in DECIDED:
        lines.append("INSERT INTO decisions (card, question, options, answer, words, asked, asked_by, answered) "
                     "VALUES (%s, %s, '[]', %s, %s, %s, 'Claude', %s);"
                     % (q(card), q(question), q(answer), q(words), q(day + "T12:00:00Z"), q(day + "T12:00:00Z")))

    for card, question, options in OPEN:
        lines.append("INSERT INTO decisions (card, question, options, asked, asked_by) VALUES (%s, %s, %s, %s, 'Claude');"
                     % (q(card), q(question), q(json.dumps(options)), q(at)))

    lines.append("INSERT INTO changes (at, author, kind, card, data) VALUES (%s, 'Claude', 'import', NULL, %s);"
                 % (q(at), q(json.dumps({"cards": len(items), "from": "the live roadmap list"}))))

    with open(out, "w") as f:
        f.write("\n".join(lines) + "\n")

    counts = {c: len(v) for c, v in by_col.items()}
    print("import: %d cards %s, %d decisions decided, %d open, into %s"
          % (len(items), counts, len(DECIDED), len(OPEN), out))


if __name__ == "__main__":
    main()
