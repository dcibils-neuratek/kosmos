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
  removed     TEXT                          -- set when removed; kept 30 days
);

CREATE INDEX IF NOT EXISTS cards_col ON cards (col, rank);

-- A note on a card, signed by the key that wrote it.
CREATE TABLE IF NOT EXISTS notes (
  id    INTEGER PRIMARY KEY AUTOINCREMENT,
  card  TEXT NOT NULL,
  text  TEXT NOT NULL,
  author TEXT NOT NULL,
  at    TEXT NOT NULL
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
