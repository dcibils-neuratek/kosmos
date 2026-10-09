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

**What Claude is doing is on the board** (Diego, 9 October: "can we have a
way to know what claude is working on in the kanban dashboard?"): a strip
across the top with its card, its step, what it is waiting on and for how
long, a "Claude is here" mark on that card, and today's work under it -
said through `PUT /api/now` at each step, and kept in the changes.

**And what comes after it: Claude's queue** (Diego, 9 October: "We need a
queue to lay out the work plan beyond the kanban cards", "The kanban
basically nurtures the queue"). The work plan in order - cards from the
board and tasks that are no card - which Diego reorders, takes things out
of, and adds to, and from whose top Claude takes the next piece when a card
is done. A queued card is in Next, in the queue's order; one taken out goes
back to Agreed; one done, parked or removed leaves it.

**And where ideas are talked through first: Discussions** (Diego, 9
October: "a discussions page where we can brainstorm ideas in an orderly way
and structure that then the become features"), and **talk on any card**
from any device ("write a thought on a card from your phone, Claude answers
on the card"). A discussion goes open, shaping, ready, then becomes cards
or is parked; it holds the idea in Diego's words, what it is for, a summary
Claude keeps, the options with what each gains and costs, and the questions
still open. Who owes a reply is worked out from the last word: Diego's
waits for Claude, and Claude's waits for Diego only when it asks.

**And files** (Diego, 9 October: "make sure our board is able to have
attachments like screenshots for references and for claude to upload to
cards for showing progress, finished work"): pictures, PDFs, short films
and text on a card or a discussion, each with a caption, shown in its
talk; at most 15 MB, kept in the database in 512 KB pieces, so no second
service (R2 would want a payment method on the account).

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
| POST | `/api/cards/:id/notes` | `{ text, asks }`: a card's talk; `asks` when Claude awaits Diego's reply |
| DELETE | `/api/cards/:id` | removed, kept to bring back |
| GET | `/api/decisions?open=true` | the questions, open or all |
| POST | `/api/decisions` | `{ card, question, options }` |
| PATCH | `/api/decisions/:id` | `{ answer, words }`: answered, and a note on its card |
| GET | `/api/now` | what Claude is doing now: its card, its step, what it waits on, since when |
| PUT | `/api/now` | `{ card, step, state, waiting }`: said by Claude at each step; `state` is working, waiting or idle |
| GET | `/api/queue` | Claude's queue in order: cards and tasks, each with its place |
| POST | `/api/queue` | `{ card }` or `{ title }` for a task; last, `{ before }` or `{ top: true }` |
| POST | `/api/queue/:id/move` | `{ before }`, `{ top: true }`, or last |
| DELETE | `/api/queue/:id` | out of the queue; a card waiting in Next goes back to Agreed |
| POST | `/api/queue/take` | Claude takes the top: it becomes now, its card Building |
| GET | `/api/discussions?stage=` | open, shaping, ready, became, parked |
| POST | `/api/discussions` | `{ title, idea, for }` - the idea is its first message |
| GET | `/api/discussions/:id` | with its summary, options, open questions, conversation and cards |
| PATCH | `/api/discussions/:id` | `stage`, `summary`, `options`, `questions`, `title`, `idea`, `for` |
| POST | `/api/discussions/:id/messages` | `{ text, asks }` |
| POST | `/api/discussions/:id/cards` | a card made from it, linked both ways; it has become cards |
| GET | `/api/waiting` | what waits for a reply from Claude, and from Diego |
| POST | `/api/cards/:id/files` | a file onto a card: the bytes as the body, `Content-Type`, `?name=&caption=` |
| POST | `/api/discussions/:id/files` | a file onto a discussion, the same way |
| GET | `/api/files/:id` | the file; `DELETE` removes it |
| GET | `/api/changes?since=` | everything changed since a time |
| GET | `/api/export.md` | the board as Markdown |

## Where it is

**the board's address (kept on the Mac, ~/.config/kosmos-board/url)**, deployed 9 October 2026 from
Diego's own Cloudflare account. The account's number is not in this
repository: it is in `~/.config/kosmos-board/account` on the Mac, which
`keys.py` reads, and a deploy is

    CLOUDFLARE_ACCOUNT_ID=$(cat ~/.config/kosmos-board/account) wrangler deploy

from this folder - asked of Diego before each one. Claude's key is
`~/.config/kosmos-board/claude.key`.

**A key for each of Diego's devices**, shown once in his own Terminal:
`python3 tools/board/keys.py make "Diego's phone" --show`, pasted into the
page on that device. `keys.py list` and `keys.py revoke NAME` are the rest.

## On this Mac

    python3 tools/board/test_board.py      the API, over a local database,
                                           then the page in headless Chrome

The page is `public/index.html`, one file, no build step: a key is asked
for once on each device and kept in that browser. Cards drag with a mouse,
or with a finger after resting on one a third of a second - onto a
column, or on a phone onto its tab. A screenshot pasted while a card is
open goes onto that card. `test_page.mjs` drives it through Chrome's
DevTools protocol, real mouse drags included; `CHROME=` names another
Chrome.

`schema.sql` is the tables; `import.py` turned the live roadmap list into the
first cards (9 October 2026); `keys.py` makes and withdraws keys.
