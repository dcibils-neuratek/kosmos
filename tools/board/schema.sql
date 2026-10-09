-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The Kosmos Board's tables, in Cloudflare's D1 (SQLite). The board is
-- the roadmap and its decisions as cards; `docs/roadmap.md` stays the
-- record, with the reasoning, and is written from here.

-- A card: one thing asked for, agreed, being built or done.
CREATE TABLE IF NOT EXISTS cards (
  id          TEXT PRIMARY KEY,
  title       TEXT NOT NULL,
  col         TEXT NOT NULL,               -- ideas agreed next building done parked
  rank        REAL NOT NULL DEFAULT 0,     -- its place in its column, lowest first
  area        TEXT NOT NULL DEFAULT '',
  ref         TEXT NOT NULL DEFAULT '',    -- a roadmap step: "Mail M6", "N12"
  detail      TEXT NOT NULL DEFAULT '',
  quote       TEXT NOT NULL DEFAULT '',    -- Diego's words, when he asked
  asked       TEXT NOT NULL DEFAULT '',    -- the day he asked or agreed
  in_roadmap  INTEGER NOT NULL DEFAULT 1,  -- 0 until written into docs/roadmap.md
  created     TEXT NOT NULL,
  updated     TEXT NOT NULL,
  updated_by  TEXT NOT NULL DEFAULT '',
  removed     TEXT,                         -- set when removed; kept 30 days
  discussion  INTEGER                       -- the discussion it came from, if one
);

CREATE INDEX IF NOT EXISTS cards_col ON cards (col, rank);

-- A note on a card, signed by the key that wrote it.
CREATE TABLE IF NOT EXISTS notes (
  id    INTEGER PRIMARY KEY AUTOINCREMENT,
  card  TEXT NOT NULL,
  text  TEXT NOT NULL,
  author TEXT NOT NULL,
  at    TEXT NOT NULL,
  asks  INTEGER NOT NULL DEFAULT 0          -- Claude asking Diego something: his reply is awaited
);

CREATE INDEX IF NOT EXISTS notes_card ON notes (card, at);

-- A question for Diego, on a card or on none, and his answer.
CREATE TABLE IF NOT EXISTS decisions (
  id        INTEGER PRIMARY KEY AUTOINCREMENT,
  card      TEXT,
  question  TEXT NOT NULL,
  options   TEXT NOT NULL DEFAULT '[]',     -- JSON array of strings
  answer    TEXT,                           -- null while open
  words     TEXT NOT NULL DEFAULT '',       -- his words, as he said them
  asked     TEXT NOT NULL,
  asked_by  TEXT NOT NULL,
  answered  TEXT
);

-- Who may use the API: one key a device, one for Claude. Only a key's
-- SHA-256 is kept; the key itself is shown once, when it is made.
CREATE TABLE IF NOT EXISTS keys (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  name       TEXT NOT NULL,
  hash       TEXT NOT NULL UNIQUE,
  created    TEXT NOT NULL,
  last_used  TEXT,
  revoked    TEXT
);

-- Everything that changed, and who changed it: what /api/changes reads.
CREATE TABLE IF NOT EXISTS changes (
  id    INTEGER PRIMARY KEY AUTOINCREMENT,
  at    TEXT NOT NULL,
  author TEXT NOT NULL,
  kind  TEXT NOT NULL,                      -- card.new card.edit card.move card.remove note decision.new decision.answer
  card  TEXT,
  data  TEXT NOT NULL DEFAULT '{}'
);

CREATE INDEX IF NOT EXISTS changes_at ON changes (at);

-- What Claude is doing now: one row, set at each step and read by the
-- board's strip and the card's mark. `state` is working, waiting or idle;
-- `waiting` says on what - a test run, Diego's answer, a restart.
CREATE TABLE IF NOT EXISTS doing (
  id       INTEGER PRIMARY KEY CHECK (id = 1),
  state    TEXT NOT NULL DEFAULT 'idle',
  card     TEXT,
  step     TEXT NOT NULL DEFAULT '',
  waiting  TEXT NOT NULL DEFAULT '',
  since    TEXT NOT NULL,                  -- when this card was started
  updated  TEXT NOT NULL,                  -- when it was last said
  author   TEXT NOT NULL
);

-- Claude's queue: the work plan, in order. An entry is a card from the
-- board or a task that is no card ("Put 0.11.91 on the M700"). The board
-- feeds it - a queued card is in Next, in the queue's order - and Claude
-- takes the top when the card it is on is done.
CREATE TABLE IF NOT EXISTS queue (
  id        INTEGER PRIMARY KEY AUTOINCREMENT,
  card      TEXT UNIQUE,                   -- null for a task
  title     TEXT NOT NULL DEFAULT '',      -- a task's words; a card's title is the card's
  rank      REAL NOT NULL,
  added     TEXT NOT NULL,
  added_by  TEXT NOT NULL
);

-- Discussions: ideas talked through in order before they are cards
-- (Diego, 9 October: "a discussions page where we can brainstorm ideas in
-- an orderly way and structure that then the become features"). A stage
-- - open, shaping, ready, became, parked - the idea in Diego's words and
-- what it is for, a summary Claude keeps, the options with what each gains
-- and costs, the questions still open, and the conversation.
CREATE TABLE IF NOT EXISTS discussions (
  id        INTEGER PRIMARY KEY AUTOINCREMENT,
  title     TEXT NOT NULL,
  idea      TEXT NOT NULL DEFAULT '',
  purpose   TEXT NOT NULL DEFAULT '',
  stage     TEXT NOT NULL DEFAULT 'open',
  summary   TEXT NOT NULL DEFAULT '',
  options   TEXT NOT NULL DEFAULT '[]',     -- [{ name, plus, minus, recommended, chosen }]
  questions TEXT NOT NULL DEFAULT '[]',     -- the questions still open
  created   TEXT NOT NULL,
  author    TEXT NOT NULL,
  updated   TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS messages (
  id          INTEGER PRIMARY KEY AUTOINCREMENT,
  discussion  INTEGER NOT NULL,
  text        TEXT NOT NULL,
  author      TEXT NOT NULL,
  at          TEXT NOT NULL,
  asks        INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS messages_discussion ON messages (discussion, at);

-- Files on a card or a discussion: screenshots for reference, and what
-- Claude shows of its progress and its finished work (Diego, 9 October:
-- "make sure our board is able to have attachments like screenshots").
-- Kept in the database itself, in pieces of 512 KB - D1's rows hold 2 MB,
-- and a screenshot of the M700 is often more - so the board needs no
-- second service; at most 15 MB a file.
CREATE TABLE IF NOT EXISTS files (
  id          TEXT PRIMARY KEY,
  card        TEXT,
  discussion  INTEGER,
  name        TEXT NOT NULL,
  type        TEXT NOT NULL,
  size        INTEGER NOT NULL,
  caption     TEXT NOT NULL DEFAULT '',
  author      TEXT NOT NULL,
  at          TEXT NOT NULL,
  removed     TEXT
);

CREATE INDEX IF NOT EXISTS files_card ON files (card, at);
CREATE INDEX IF NOT EXISTS files_discussion ON files (discussion, at);

CREATE TABLE IF NOT EXISTS file_pieces (
  file  TEXT NOT NULL,
  n     INTEGER NOT NULL,
  data  BLOB NOT NULL,
  PRIMARY KEY (file, n)
);
