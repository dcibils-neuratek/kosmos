// Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
//
// The Kosmos Board: the roadmap and its decisions as cards, behind one API
// that the page, Diego's phone and Claude all use (`tools/board/README.md`).
// A Cloudflare Worker over a D1 database; the page itself is a static file
// the same Worker serves.
//
// **Nothing is readable without a key.** A key is `kb_` and 40 hex
// characters, sent as `Authorization: Bearer kb_...`; only its SHA-256 is
// kept, with a name, so every change is signed by the device or the agent
// that made it, and a key is withdrawn without touching the others.

const COLUMNS = ["ideas", "agreed", "next", "building", "done", "parked"];
const LIMITS = { title: 200, detail: 4000, quote: 400, area: 40, ref: 60, note: 4000, question: 600, words: 2000, step: 300, waiting: 200, task: 200, idea: 2000, purpose: 1000, summary: 4000 };
const STAGES = ["open", "shaping", "ready", "became", "parked"];

// The name of Claude's key: what it writes is never "waiting for Claude".
const CLAUDE = "Claude";
const STATES = ["working", "waiting", "idle"];

const now = () => new Date().toISOString();

function json(body, status = 200) {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "content-type": "application/json; charset=utf-8", "cache-control": "no-store" },
  });
}

function refused(status, why) {
  return json({ error: why }, status);
}

async function sha256(text) {
  const bytes = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text));
  return [...new Uint8Array(bytes)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

// The key's name, or null: who is asking.
async function who(request, env) {
  const auth = request.headers.get("authorization") || "";
  const m = auth.match(/^Bearer (kb_[0-9a-f]{40})$/);

  if (!m) return null;

  const hash = await sha256(m[1]);
  const row = await env.DB.prepare("SELECT id, name FROM keys WHERE hash = ? AND revoked IS NULL")
    .bind(hash).first();

  if (!row) return null;

  await env.DB.prepare("UPDATE keys SET last_used = ? WHERE id = ?").bind(now(), row.id).run();
  return row.name;
}

// A text field as it may be kept: a string, trimmed, no longer than its limit.
function text(v, field, required) {
  if (v === undefined || v === null) {
    if (required) throw new Error(`${field} is needed`);
    return undefined;
  }

  if (typeof v !== "string") throw new Error(`${field} must be words`);

  const t = v.trim();

  if (required && !t) throw new Error(`${field} is empty`);
  if (t.length > LIMITS[field]) throw new Error(`${field} is longer than ${LIMITS[field]} characters`);

  return t;
}

function column(v) {
  if (!COLUMNS.includes(v)) throw new Error(`a column is one of ${COLUMNS.join(", ")}`);
  return v;
}

function slug(title) {
  const s = title.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "").slice(0, 48) || "card";
  return `${s}-${Date.now().toString(36)}`;
}

async function log(env, author, kind, card, data) {
  await env.DB.prepare("INSERT INTO changes (at, author, kind, card, data) VALUES (?, ?, ?, ?, ?)")
    .bind(now(), author, kind, card, JSON.stringify(data || {})).run();
}

async function card(env, id) {
  return env.DB.prepare("SELECT * FROM cards WHERE id = ? AND removed IS NULL").bind(id).first();
}

// The rank that puts a card last in a column, or just before `before`.
async function rank_for(env, col, before, moving) {
  if (before) {
    const b = await env.DB.prepare("SELECT rank FROM cards WHERE id = ? AND col = ? AND removed IS NULL")
      .bind(before, col).first();

    if (!b) throw new Error(`no card ${before} in ${col}`);

    const prev = await env.DB.prepare(
      "SELECT rank FROM cards WHERE col = ? AND rank < ? AND id != ? AND removed IS NULL ORDER BY rank DESC LIMIT 1")
      .bind(col, b.rank, moving || "").first();

    return prev ? (prev.rank + b.rank) / 2 : b.rank - 1;
  }

  const last = await env.DB.prepare("SELECT MAX(rank) AS r FROM cards WHERE col = ? AND removed IS NULL")
    .bind(col).first();

  return (last && last.r !== null ? last.r : 0) + 1;
}

//
// **Who owes a reply**, from the last word in each thread: Diego's (or any
// key but Claude's) waits for Claude; Claude's waits for Diego only when it
// asked something - a note saying what was built waits for nobody.
//
async function talk_states(env, table, key) {
  const rows = await env.DB.prepare(
    `SELECT t.${key} AS k, t.author, t.asks FROM ${table} t
     JOIN (SELECT ${key}, MAX(id) AS id FROM ${table} GROUP BY ${key}) last ON last.id = t.id`).all();
  const out = new Map();

  for (const r of rows.results) {
    if (r.author !== CLAUDE) out.set(r.k, "claude");
    else if (r.asks) out.set(r.k, "diego");
  }

  return out;
}

function shape(c) {
  return { ...c, in_roadmap: !!c.in_roadmap };
}

// ---- The routes ----------------------------------------------------------

async function list_cards(env, url) {
  const where = ["removed IS NULL"];
  const args = [];
  const col = url.searchParams.get("column");
  const area = url.searchParams.get("area");
  const q = url.searchParams.get("q");

  if (col) { where.push("col = ?"); args.push(column(col)); }
  if (area) { where.push("area = ?"); args.push(area); }
  if (q) {
    where.push("(title LIKE ? OR detail LIKE ? OR ref LIKE ? OR quote LIKE ?)");
    const like = `%${q}%`;
    args.push(like, like, like, like);
  }

  // Next in the queue's order, each with its place in it; the rest by rank.
  const rows = await env.DB.prepare(
    `SELECT cards.*, q.rank AS queue_rank FROM cards LEFT JOIN queue q ON q.card = cards.id
     WHERE ${where.join(" AND ").replace(/\b(col|area|title|detail|ref|quote|removed)\b/g, "cards.$1")}
     ORDER BY cards.col, CASE WHEN cards.col = 'next' THEN q.rank END, cards.rank`)
    .bind(...args).all();
  const talk = await talk_states(env, "notes", "card");

  const queue = await env.DB.prepare("SELECT card FROM queue ORDER BY rank").all();
  const place = new Map(queue.results.map((e, i) => [e.card, i + 1]));

  return json({ cards: rows.results.map(({ queue_rank, ...c }) => ({
    ...shape(c), talk: talk.get(c.id) || null, queued: place.get(c.id) || null })) });
}

async function new_card(env, author, body) {
  const title = text(body.title, "title", true);
  const col = column(body.column || "ideas");
  const id = slug(title);
  const t = now();

  await env.DB.prepare(`INSERT INTO cards (id, title, col, rank, area, ref, detail, quote, asked, in_roadmap,
                        created, updated, updated_by) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 0, ?, ?, ?)`)
    .bind(id, title, col, await rank_for(env, col, body.before), text(body.area, "area") || "",
          text(body.ref, "ref") || "", text(body.detail, "detail") || "", text(body.quote, "quote") || "",
          t.slice(0, 10), t, t, author).run();

  // Made straight into Next, it is queued, last: Next is the queue.
  if (col === "next") {
    await env.DB.prepare("INSERT INTO queue (card, title, rank, added, added_by) VALUES (?, '', ?, ?, ?)")
      .bind(id, await queue_rank(env), t, author).run();
  }

  await log(env, author, "card.new", id, { title, column: col });
  return json({ card: shape(await card(env, id)) }, 201);
}

async function get_card(env, id) {
  const c = await card(env, id);

  if (!c) return refused(404, `no card ${id}`);

  const notes = await env.DB.prepare("SELECT id, text, author, at FROM notes WHERE card = ? ORDER BY at").bind(id).all();
  const history = await env.DB.prepare("SELECT at, author, kind, data FROM changes WHERE card = ? ORDER BY at").bind(id).all();
  const decisions = await env.DB.prepare("SELECT * FROM decisions WHERE card = ? ORDER BY asked").bind(id).all();

  return json({
    card: shape(c), notes: notes.results,
    history: history.results.map((h) => ({ ...h, data: JSON.parse(h.data) })),
    decisions: decisions.results.map((d) => ({ ...d, options: JSON.parse(d.options) })),
  });
}

async function edit_card(env, author, id, body) {
  const c = await card(env, id);

  if (!c) return refused(404, `no card ${id}`);

  const fields = {};

  for (const f of ["title", "area", "ref", "detail", "quote"]) {
    const v = text(body[f], f, f === "title" && body.title !== undefined);
    if (v !== undefined) fields[f] = v;
  }

  if (body.in_roadmap !== undefined) fields.in_roadmap = body.in_roadmap ? 1 : 0;

  const names = Object.keys(fields);

  if (!names.length) return refused(400, "nothing to change");

  await env.DB.prepare(`UPDATE cards SET ${names.map((n) => `${n} = ?`).join(", ")}, updated = ?, updated_by = ? WHERE id = ?`)
    .bind(...names.map((n) => fields[n]), now(), author, id).run();

  await log(env, author, "card.edit", id, fields);
  return json({ card: shape(await card(env, id)) });
}

async function move_card(env, author, id, body) {
  const c = await card(env, id);

  if (!c) return refused(404, `no card ${id}`);

  const col = column(body.column || c.col);

  if (body.before === id) return refused(400, "a card cannot go before itself");

  const r = await rank_for(env, col, body.before, id);

  await env.DB.prepare("UPDATE cards SET col = ?, rank = ?, updated = ?, updated_by = ? WHERE id = ?")
    .bind(col, r, now(), author, id).run();

  // **Next is Claude's queue, as cards** (Diego, 9 October: "i need to be
  // able to drag and drop cards", "like a real kanban board"): dropped
  // into Next, a card is queued where it was dropped - before the entry of
  // the card it landed above, or last; moved within Next, its entry moves;
  // taken out to anywhere but Building, it leaves the queue.
  const queued = await env.DB.prepare("SELECT id FROM queue WHERE card = ?").bind(id).first();

  if (col === "next") {
    const above = body.before
      ? await env.DB.prepare("SELECT id FROM queue WHERE card = ?").bind(body.before).first() : null;
    const place = await queue_rank(env, above ? above.id : null, false, queued ? queued.id : 0);

    if (queued) {
      await env.DB.prepare("UPDATE queue SET rank = ? WHERE id = ?").bind(place, queued.id).run();
    } else {
      await env.DB.prepare("INSERT INTO queue (card, title, rank, added, added_by) VALUES (?, '', ?, ?, ?)")
        .bind(id, place, now(), author).run();
    }
  } else if (queued && col !== "building") {
    await env.DB.prepare("DELETE FROM queue WHERE id = ?").bind(queued.id).run();
  }

  await log(env, author, "card.move", id, { from: c.col, to: col, before: body.before || null });
  return json({ card: shape(await card(env, id)) });
}

async function remove_card(env, author, id) {
  const c = await card(env, id);

  if (!c) return refused(404, `no card ${id}`);

  await env.DB.prepare("UPDATE cards SET removed = ?, updated = ?, updated_by = ? WHERE id = ?")
    .bind(now(), now(), author, id).run();

  await env.DB.prepare("DELETE FROM queue WHERE card = ?").bind(id).run();
  await log(env, author, "card.remove", id, { title: c.title });
  return json({ removed: id });
}

async function add_note(env, author, id, body) {
  if (!(await card(env, id))) return refused(404, `no card ${id}`);

  const t = text(body.text, "note", true);
  const asks = body.asks ? 1 : 0;
  const at = now();

  await env.DB.prepare("INSERT INTO notes (card, text, author, at, asks) VALUES (?, ?, ?, ?, ?)").bind(id, t, author, at, asks).run();
  await log(env, author, "note", id, { text: t, asks: !!asks });
  return json({ note: { card: id, text: t, author, at, asks: !!asks } }, 201);
}

async function list_decisions(env, url) {
  const open = url.searchParams.get("open") === "true";
  const rows = await env.DB.prepare(
    `SELECT * FROM decisions ${open ? "WHERE answer IS NULL" : ""} ORDER BY asked DESC`).all();

  return json({ decisions: rows.results.map((d) => ({ ...d, options: JSON.parse(d.options) })) });
}

async function new_decision(env, author, body) {
  const question = text(body.question, "question", true);
  const options = Array.isArray(body.options) ? body.options.slice(0, 8).map((o) => text(o, "title", true)) : [];

  if (body.card && !(await card(env, body.card))) return refused(404, `no card ${body.card}`);

  const r = await env.DB.prepare("INSERT INTO decisions (card, question, options, asked, asked_by) VALUES (?, ?, ?, ?, ?)")
    .bind(body.card || null, question, JSON.stringify(options), now(), author).run();

  await log(env, author, "decision.new", body.card || null, { question, options });
  return json({ decision: { id: r.meta.last_row_id, card: body.card || null, question, options, answer: null } }, 201);
}

async function answer_decision(env, author, id, body) {
  const d = await env.DB.prepare("SELECT * FROM decisions WHERE id = ?").bind(id).first();

  if (!d) return refused(404, `no decision ${id}`);

  const answer = text(body.answer, "question", true);
  const words = text(body.words, "words") || "";
  const at = now();

  await env.DB.prepare("UPDATE decisions SET answer = ?, words = ?, answered = ? WHERE id = ?")
    .bind(answer, words, at, id).run();

  if (d.card) {
    await env.DB.prepare("INSERT INTO notes (card, text, author, at) VALUES (?, ?, ?, ?)")
      .bind(d.card, `Decided: ${d.question} - ${answer}${words ? ` ("${words}")` : ""}`, author, at).run();
  }

  await log(env, author, "decision.answer", d.card, { question: d.question, answer, words });
  return json({ decision: { ...d, options: JSON.parse(d.options), answer, words, answered: at } });
}

//
// **What Claude is doing now** (Diego, 9 October: "can we have a way to know
// what claude is working on in the kanban dashboard?"): its card, its step,
// what it waits on, and since when - one row, said at each step. `since`
// moves only when the card does, so the board says how long a card has
// taken, not how long since the last word.
//
async function get_now(env) {
  const row = await env.DB.prepare("SELECT * FROM doing WHERE id = 1").first();

  if (!row) return json({ now: { state: "idle", card: null, step: "", waiting: "", since: null, updated: null } });

  const c = row.card ? await card(env, row.card) : null;

  return json({ now: { ...row, title: c ? c.title : null } });
}

async function set_now(env, author, body) {
  const state = body.state || "working";

  if (!STATES.includes(state)) throw new Error(`a state is one of ${STATES.join(", ")}`);
  if (body.card && !(await card(env, body.card))) return refused(404, `no card ${body.card}`);

  const step = text(body.step, "step") || "";
  const waiting = text(body.waiting, "waiting") || "";
  const at = now();
  const before = await env.DB.prepare("SELECT card, since FROM doing WHERE id = 1").first();
  const since = before && before.card === (body.card || null) && before.since ? before.since : at;

  await env.DB.prepare(`INSERT INTO doing (id, state, card, step, waiting, since, updated, author)
                        VALUES (1, ?, ?, ?, ?, ?, ?, ?)
                        ON CONFLICT (id) DO UPDATE SET state = excluded.state, card = excluded.card,
                        step = excluded.step, waiting = excluded.waiting, since = excluded.since,
                        updated = excluded.updated, author = excluded.author`)
    .bind(state, body.card || null, step, waiting, since, at, author).run();

  await log(env, author, "now", body.card || null, { state, step, waiting });
  return get_now(env);
}

//
// **Claude's queue** (Diego, 9 October: "We need a queue to lay out the work
// plan beyond the kanban cards", "The kanban basically nurtures the
// queue"): what comes after the card Claude is on, in order. A card in it is
// in Next; out of it, back in Agreed; a task is an entry with no card.
//
async function queue_rank(env, before, top, moving) {
  if (top) {
    const first = await env.DB.prepare("SELECT MIN(rank) AS r FROM queue WHERE id != ?").bind(moving || 0).first();
    return (first && first.r !== null ? first.r : 1) - 1;
  }

  if (before) {
    const b = await env.DB.prepare("SELECT rank FROM queue WHERE id = ?").bind(before).first();

    if (!b) throw new Error(`no queue entry ${before}`);

    const prev = await env.DB.prepare("SELECT rank FROM queue WHERE rank < ? AND id != ? ORDER BY rank DESC LIMIT 1")
      .bind(b.rank, moving || 0).first();

    return prev ? (prev.rank + b.rank) / 2 : b.rank - 1;
  }

  const last = await env.DB.prepare("SELECT MAX(rank) AS r FROM queue WHERE id != ?").bind(moving || 0).first();
  return (last && last.r !== null ? last.r : 0) + 1;
}

async function get_queue(env) {
  const rows = await env.DB.prepare(
    `SELECT q.id, q.card, q.title AS task, q.rank, q.added, q.added_by, c.title, c.area, c.col, c.ref
     FROM queue q LEFT JOIN cards c ON c.id = q.card ORDER BY q.rank`).all();

  return json({ queue: rows.results.map((r, i) => ({
    id: r.id, place: i + 1, kind: r.card ? "card" : "task", card: r.card,
    title: r.card ? r.title : r.task, area: r.area || "", column: r.col || null, ref: r.ref || "",
    added: r.added, added_by: r.added_by,
  })) });
}

async function enqueue(env, author, body) {
  const at = now();
  let title = "";

  if (body.card) {
    const c = await card(env, body.card);

    if (!c) return refused(404, `no card ${body.card}`);
    if (c.col === "done") return refused(400, "a done card is not queued");

    const there = await env.DB.prepare("SELECT id FROM queue WHERE card = ?").bind(body.card).first();

    if (there) return refused(400, `${body.card} is in the queue already`);

    if (c.col !== "next" && c.col !== "building") {
      await env.DB.prepare("UPDATE cards SET col = 'next', rank = ?, updated = ?, updated_by = ? WHERE id = ?")
        .bind(await rank_for(env, "next"), at, author, body.card).run();
    }
  } else {
    title = text(body.title, "task", true);
  }

  const r = await env.DB.prepare("INSERT INTO queue (card, title, rank, added, added_by) VALUES (?, ?, ?, ?, ?)")
    .bind(body.card || null, title, await queue_rank(env, body.before, body.top), at, author).run();

  await log(env, author, "queue.add", body.card || null, { entry: r.meta.last_row_id, task: title || undefined });
  return get_queue(env);
}

async function move_queued(env, author, id, body) {
  const e = await env.DB.prepare("SELECT * FROM queue WHERE id = ?").bind(id).first();

  if (!e) return refused(404, `no queue entry ${id}`);
  if (body.before === id) return refused(400, "an entry cannot go before itself");

  await env.DB.prepare("UPDATE queue SET rank = ? WHERE id = ?")
    .bind(await queue_rank(env, body.before, body.top, id), id).run();

  await log(env, author, "queue.move", e.card, { entry: id, before: body.before || null, top: !!body.top });
  return get_queue(env);
}

// Out of the queue: a card that was waiting in Next goes back to Agreed.
async function dequeue(env, author, id, reason) {
  const e = await env.DB.prepare("SELECT * FROM queue WHERE id = ?").bind(id).first();

  if (!e) return refused(404, `no queue entry ${id}`);

  await env.DB.prepare("DELETE FROM queue WHERE id = ?").bind(id).run();

  if (e.card && !reason) {
    const c = await card(env, e.card);

    if (c && c.col === "next") {
      await env.DB.prepare("UPDATE cards SET col = 'agreed', rank = ?, updated = ?, updated_by = ? WHERE id = ?")
        .bind(await rank_for(env, "agreed"), now(), author, e.card).run();
    }
  }

  await log(env, author, "queue.remove", e.card, { entry: id, task: e.title || undefined, reason: reason || null });
  return get_queue(env);
}

// Claude takes the top: it is what Claude is doing now, and its card is
// in Building.
async function take(env, author) {
  const e = await env.DB.prepare("SELECT * FROM queue ORDER BY rank LIMIT 1").first();

  if (!e) return refused(404, "the queue is empty");

  await env.DB.prepare("DELETE FROM queue WHERE id = ?").bind(e.id).run();

  if (e.card) {
    await env.DB.prepare("UPDATE cards SET col = 'building', rank = ?, updated = ?, updated_by = ? WHERE id = ?")
      .bind(await rank_for(env, "building"), now(), author, e.card).run();
  }

  await log(env, author, "queue.take", e.card, { entry: e.id, task: e.title || undefined });
  return set_now(env, author, { card: e.card || undefined, step: e.card ? "" : e.title, state: "working" });
}

function discussion_shape(d, talk) {
  return { ...d, options: JSON.parse(d.options), questions: JSON.parse(d.questions), talk: talk || null };
}

function options_of(v) {
  if (!Array.isArray(v)) throw new Error("options are a list");

  return v.slice(0, 8).map((o) => ({
    name: text(o && o.name, "title", true), plus: text(o.plus, "purpose") || "", minus: text(o.minus, "purpose") || "",
    recommended: !!o.recommended, chosen: !!o.chosen,
  }));
}

function questions_of(v) {
  if (!Array.isArray(v)) throw new Error("questions are a list");
  return v.slice(0, 12).map((q) => text(q, "question", true));
}

async function list_discussions(env, url) {
  const stage = url.searchParams.get("stage");

  if (stage && !STAGES.includes(stage)) throw new Error(`a stage is one of ${STAGES.join(", ")}`);

  const rows = await env.DB.prepare(`SELECT * FROM discussions ${stage ? "WHERE stage = ?" : ""} ORDER BY updated DESC`)
    .bind(...(stage ? [stage] : [])).all();
  const talk = await talk_states(env, "messages", "discussion");

  return json({ discussions: rows.results.map((d) => discussion_shape(d, talk.get(d.id))) });
}

async function new_discussion(env, author, body) {
  const t = now();
  const r = await env.DB.prepare(
    "INSERT INTO discussions (title, idea, purpose, created, author, updated) VALUES (?, ?, ?, ?, ?, ?)")
    .bind(text(body.title, "title", true), text(body.idea, "idea") || "", text(body.for, "purpose") || "", t, author, t).run();
  const id = r.meta.last_row_id;

  if (body.idea) {
    await env.DB.prepare("INSERT INTO messages (discussion, text, author, at) VALUES (?, ?, ?, ?)")
      .bind(id, text(body.idea, "idea"), author, t).run();
  }

  await log(env, author, "discussion.new", null, { discussion: id, title: body.title });
  return get_discussion(env, id, 201);
}

async function get_discussion(env, id, status = 200) {
  const d = await env.DB.prepare("SELECT * FROM discussions WHERE id = ?").bind(id).first();

  if (!d) return refused(404, `no discussion ${id}`);

  const msgs = await env.DB.prepare("SELECT id, text, author, at, asks FROM messages WHERE discussion = ? ORDER BY id").bind(id).all();
  const cards = await env.DB.prepare("SELECT id, title, col FROM cards WHERE discussion = ? AND removed IS NULL").bind(id).all();
  const talk = await talk_states(env, "messages", "discussion");

  return json({ discussion: discussion_shape(d, talk.get(d.id)),
                messages: msgs.results.map((m) => ({ ...m, asks: !!m.asks })), cards: cards.results }, status);
}

async function edit_discussion(env, author, id, body) {
  const d = await env.DB.prepare("SELECT * FROM discussions WHERE id = ?").bind(id).first();

  if (!d) return refused(404, `no discussion ${id}`);

  const fields = {};

  if (body.title !== undefined) fields.title = text(body.title, "title", true);
  if (body.idea !== undefined) fields.idea = text(body.idea, "idea");
  if (body.for !== undefined) fields.purpose = text(body.for, "purpose");
  if (body.summary !== undefined) fields.summary = text(body.summary, "summary");
  if (body.options !== undefined) fields.options = JSON.stringify(options_of(body.options));
  if (body.questions !== undefined) fields.questions = JSON.stringify(questions_of(body.questions));
  if (body.stage !== undefined) {
    if (!STAGES.includes(body.stage)) throw new Error(`a stage is one of ${STAGES.join(", ")}`);
    fields.stage = body.stage;
  }

  const names = Object.keys(fields);

  if (!names.length) return refused(400, "nothing to change");

  await env.DB.prepare(`UPDATE discussions SET ${names.map((n) => `${n} = ?`).join(", ")}, updated = ? WHERE id = ?`)
    .bind(...names.map((n) => fields[n]), now(), id).run();

  await log(env, author, "discussion.edit", null, { discussion: id, fields: names });
  return get_discussion(env, id);
}

async function add_message(env, author, id, body) {
  if (!(await env.DB.prepare("SELECT id FROM discussions WHERE id = ?").bind(id).first())) {
    return refused(404, `no discussion ${id}`);
  }

  const t = text(body.text, "note", true);
  const at = now();

  await env.DB.prepare("INSERT INTO messages (discussion, text, author, at, asks) VALUES (?, ?, ?, ?, ?)")
    .bind(id, t, author, at, body.asks ? 1 : 0).run();
  await env.DB.prepare("UPDATE discussions SET updated = ? WHERE id = ?").bind(at, id).run();
  await log(env, author, "message", null, { discussion: id, text: t, asks: !!body.asks });
  return get_discussion(env, id, 201);
}

// A card made from a discussion: linked both ways, and the discussion has
// become cards.
async function discussion_card(env, author, id, body) {
  const d = await env.DB.prepare("SELECT * FROM discussions WHERE id = ?").bind(id).first();

  if (!d) return refused(404, `no discussion ${id}`);

  const res = await new_card(env, author, { ...body, detail: body.detail || d.summary, quote: body.quote || d.idea });
  const made = (await res.clone().json()).card;

  await env.DB.prepare("UPDATE cards SET discussion = ? WHERE id = ?").bind(id, made.id).run();
  await env.DB.prepare("UPDATE discussions SET stage = 'became', updated = ? WHERE id = ?").bind(now(), id).run();
  await log(env, author, "discussion.card", made.id, { discussion: id });
  return get_discussion(env, id, 201);
}

// What waits for a reply, and from whom: Claude's first look.
async function waiting(env) {
  const cards = await talk_states(env, "notes", "card");
  const talks = await talk_states(env, "messages", "discussion");
  const out = { claude: { cards: [], discussions: [] }, diego: { cards: [], discussions: [] } };

  for (const [id, who] of cards) {
    const c = await card(env, id);
    if (c) out[who].cards.push({ id, title: c.title });
  }

  for (const [id, who] of talks) {
    const d = await env.DB.prepare("SELECT id, title FROM discussions WHERE id = ?").bind(id).first();
    if (d) out[who].discussions.push(d);
  }

  const open = await env.DB.prepare("SELECT id, card, question FROM decisions WHERE answer IS NULL").all();

  out.diego.decisions = open.results;
  return json(out);
}

async function changes(env, url) {
  const since = url.searchParams.get("since") || "1970-01-01T00:00:00Z";
  const rows = await env.DB.prepare("SELECT * FROM changes WHERE at > ? ORDER BY at LIMIT 1000").bind(since).all();

  return json({ changes: rows.results.map((c) => ({ ...c, data: JSON.parse(c.data) })), now: now() });
}

const NAMES = { ideas: "Ideas", agreed: "Agreed", next: "Next", building: "Building", done: "Done", parked: "Parked" };

async function export_md(env) {
  const rows = await env.DB.prepare("SELECT * FROM cards WHERE removed IS NULL ORDER BY col, rank").all();
  const out = ["# Kosmos Board", "", `Exported ${now()}.`, ""];

  for (const col of COLUMNS) {
    const list = rows.results.filter((c) => c.col === col);

    out.push(`## ${NAMES[col]} (${list.length})`, "");

    for (const c of list) {
      out.push(`- **${c.title}**${c.area ? ` · ${c.area}` : ""}${c.ref ? ` · ${c.ref}` : ""}${c.in_roadmap ? "" : " · not yet in roadmap.md"}`);
      if (c.detail) out.push(`  ${c.detail}`);
      if (c.quote) out.push(`  > ${c.quote}`);
    }

    out.push("");
  }

  return new Response(out.join("\n"), { headers: { "content-type": "text/markdown; charset=utf-8", "cache-control": "no-store" } });
}

async function api(request, env, url) {
  const author = await who(request, env);

  if (!author) return refused(401, "a key is needed: Authorization: Bearer kb_...");

  const parts = url.pathname.replace(/^\/api\/?/, "").split("/").filter(Boolean).map(decodeURIComponent);
  const m = request.method;
  let body = {};

  if (m === "POST" || m === "PATCH" || m === "PUT") {
    // No body at all is nothing to say - a take, a move to the end.
    const raw = await request.text();

    try {
      body = raw.trim() ? JSON.parse(raw) : {};
    } catch {
      return refused(400, "the body is not JSON");
    }

    if (typeof body !== "object" || body === null || Array.isArray(body)) return refused(400, "the body is not an object");
  }

  try {
    if (parts[0] === "me" && m === "GET") return json({ name: author });
    if (parts[0] === "cards") {
      if (parts.length === 1 && m === "GET") return await list_cards(env, url);
      if (parts.length === 1 && m === "POST") return await new_card(env, author, body);
      if (parts.length === 2 && m === "GET") return await get_card(env, parts[1]);
      if (parts.length === 2 && m === "PATCH") return await edit_card(env, author, parts[1], body);
      if (parts.length === 2 && m === "DELETE") return await remove_card(env, author, parts[1]);
      if (parts.length === 3 && parts[2] === "move" && m === "POST") return await move_card(env, author, parts[1], body);
      if (parts.length === 3 && parts[2] === "notes" && m === "POST") return await add_note(env, author, parts[1], body);
    }
    if (parts[0] === "decisions") {
      if (parts.length === 1 && m === "GET") return await list_decisions(env, url);
      if (parts.length === 1 && m === "POST") return await new_decision(env, author, body);
      if (parts.length === 2 && m === "PATCH") return await answer_decision(env, author, Number(parts[1]), body);
    }
    if (parts[0] === "queue") {
      if (parts.length === 1 && m === "GET") return await get_queue(env);
      if (parts.length === 1 && m === "POST") return await enqueue(env, author, body);
      if (parts.length === 2 && parts[1] === "take" && m === "POST") return await take(env, author);
      if (parts.length === 2 && m === "DELETE") return await dequeue(env, author, Number(parts[1]));
      if (parts.length === 3 && parts[2] === "move" && m === "POST") return await move_queued(env, author, Number(parts[1]), body);
    }
    if (parts[0] === "discussions") {
      if (parts.length === 1 && m === "GET") return await list_discussions(env, url);
      if (parts.length === 1 && m === "POST") return await new_discussion(env, author, body);
      if (parts.length === 2 && m === "GET") return await get_discussion(env, Number(parts[1]));
      if (parts.length === 2 && m === "PATCH") return await edit_discussion(env, author, Number(parts[1]), body);
      if (parts.length === 3 && parts[2] === "messages" && m === "POST") return await add_message(env, author, Number(parts[1]), body);
      if (parts.length === 3 && parts[2] === "cards" && m === "POST") return await discussion_card(env, author, Number(parts[1]), body);
    }
    if (parts[0] === "waiting" && m === "GET") return await waiting(env);
    if (parts[0] === "now" && m === "GET") return await get_now(env);
    if (parts[0] === "now" && m === "PUT") return await set_now(env, author, body);
    if (parts[0] === "changes" && m === "GET") return await changes(env, url);
    if (parts[0] === "export.md" && m === "GET") return await export_md(env);
  } catch (e) {
    return refused(400, e.message);
  }

  return refused(404, `no ${m} ${url.pathname}`);
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);

    if (url.pathname.startsWith("/api/") || url.pathname === "/api") return api(request, env, url);

    return env.ASSETS.fetch(request);
  },
};
