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
const LIMITS = { title: 200, detail: 4000, quote: 400, area: 40, ref: 60, note: 4000, question: 600, words: 2000 };

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

  const rows = await env.DB.prepare(`SELECT * FROM cards WHERE ${where.join(" AND ")} ORDER BY col, rank`)
    .bind(...args).all();

  return json({ cards: rows.results.map(shape) });
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

  await log(env, author, "card.move", id, { from: c.col, to: col, before: body.before || null });
  return json({ card: shape(await card(env, id)) });
}

async function remove_card(env, author, id) {
  const c = await card(env, id);

  if (!c) return refused(404, `no card ${id}`);

  await env.DB.prepare("UPDATE cards SET removed = ?, updated = ?, updated_by = ? WHERE id = ?")
    .bind(now(), now(), author, id).run();

  await log(env, author, "card.remove", id, { title: c.title });
  return json({ removed: id });
}

async function add_note(env, author, id, body) {
  if (!(await card(env, id))) return refused(404, `no card ${id}`);

  const t = text(body.text, "note", true);
  const at = now();

  await env.DB.prepare("INSERT INTO notes (card, text, author, at) VALUES (?, ?, ?, ?)").bind(id, t, author, at).run();
  await log(env, author, "note", id, { text: t });
  return json({ note: { card: id, text: t, author, at } }, 201);
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

  if (m === "POST" || m === "PATCH") {
    try {
      body = await request.json();
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
