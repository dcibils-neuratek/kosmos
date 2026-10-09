<!-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. -->
# The Kosmos Board

The roadmap and its decisions as cards on a board: Ideas, Agreed, Next (in
order), Building, Done and Parked, with notes, history and the questions
waiting for Diego. Diego, 9 October 2026: "Can we convert that into a kanban
style simple web app I can work in and add new features and prioritize
items?", "You need to be able to read it, modify it, mark done", and "design
an api to access it as well so I can use it from my mobile and desktop
computer and you can access it". Drawn first:
https://claude.ai/artifact/DWT7DQk8hqQv6vJnrXDpiW.

**Where it runs**: Cloudflare - one Worker for the API and the page, one D1
database, both on the free tier (Diego chose Cloudflare and this folder).
**The code is here and public; the roadmap is not**: its cards live only in
the database, and what was imported came from `build/`, which git ignores.

`docs/roadmap.md` stays the record, with the reasoning; the board is where
the work is moved and decided, and what changes there is written back.

## The API

Every request carries a key, `Authorization: Bearer kb_...`: one a device and
one for Claude, each named, withdrawn on its own. Every change is kept with
the name of the key that made it.

| | | |
|---|---|---|
| GET | `/api/me` | the key's name |
| GET | `/api/cards?column=&area=&q=` | the cards, each column in order |
| POST | `/api/cards` | a new card: `title`, `column` (Ideas when not said), `area`, `ref`, `detail`, `quote`, `before` |
| GET | `/api/cards/:id` | one card, its notes, its history, its decisions |
| PATCH | `/api/cards/:id` | `title`, `area`, `ref`, `detail`, `quote`, `in_roadmap` |
| POST | `/api/cards/:id/move` | `{ column, before }`: into a column, before a card or last |
| POST | `/api/cards/:id/notes` | `{ text }` |
| DELETE | `/api/cards/:id` | removed, kept to bring back |
| GET | `/api/decisions?open=true` | the questions, open or all |
| POST | `/api/decisions` | `{ card, question, options }` |
| PATCH | `/api/decisions/:id` | `{ answer, words }`: answered, and a note on its card |
| GET | `/api/changes?since=` | everything changed since a time |
| GET | `/api/export.md` | the board as Markdown |

## Setting it up - Diego's steps, once

1. Make a free Cloudflare account at https://dash.cloudflare.com/sign-up -
   yours; nobody else signs up or signs in for you.
2. In a Terminal on this Mac: `wrangler login`, and allow it in the browser
   page it opens.
3. Say so to Claude, who then makes the database, puts the roadmap in, makes
   its own key into `~/.config/kosmos-board/claude.key`, and asks before the
   first deploy.
4. A key for each of your devices, shown once in your own Terminal:
   `python3 tools/board/keys.py make "Diego's phone" --show` - paste it into
   the board's page on that device.

## On this Mac

    python3 tools/board/test_board.py      the API, over a local database

`schema.sql` is the tables; `import.py` turned the live roadmap list into the
first cards (9 October 2026); `keys.py` makes and withdraws keys.
