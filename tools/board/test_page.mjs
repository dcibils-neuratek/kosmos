//  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
// The Kosmos Board's page, in a real browser: headless Chrome driven over
// its DevTools protocol (Node's own WebSocket), against the `wrangler dev`
// test_board.py has running.
//
//     node test_page.mjs BASE KEY CLAUDE_KEY CHROME
//
// Checked: without a key the page asks for one; with one, six columns and
// the cards in them; Claude's strip names the card in hand and leaves it
// out of Up next; a card dragged with the mouse from Ideas to Agreed and
// back again, and above another in its column, each kept by the board; a
// card opened shows its picture, fetched with the key; a note sent from
// the card; and an entry in Claude's queue dragged above another. Prints
// one line a failure and "checks N" at the end; exits 1 on any failure.

import { spawn } from "node:child_process";
import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const [BASE, KEY, CLAUDE, CHROME] = process.argv.slice(2);
const fails = [];
let checks = 0;
const check = (ok, what) => { checks++; if (!ok) fails.push(what); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function api(method, path, body, key = KEY, type) {
  const headers = { Authorization: "Bearer " + key };
  if (body !== undefined) headers["Content-Type"] = type || "application/json";
  const r = await fetch(BASE + path, { method, headers, body: body === undefined ? undefined : type ? body : JSON.stringify(body) });
  return r.json();
}

// ---- the browser ----

const profile = mkdtempSync(join(tmpdir(), "kosmos-board-page-"));
const chrome = spawn(CHROME, ["--headless=new", "--remote-debugging-port=0", "--user-data-dir=" + profile,
  "--no-first-run", "--no-default-browser-check", "--disable-gpu", "about:blank"], { stdio: ["ignore", "ignore", "pipe"] });
const port = await new Promise((resolve, reject) => {
  let said = "";
  chrome.stderr.on("data", (d) => {
    said += d;
    const m = said.match(/DevTools listening on ws:\/\/[^:]+:(\d+)\//);
    if (m) resolve(m[1]);
  });
  chrome.on("exit", () => reject(new Error("Chrome left: " + said.slice(-400))));
  setTimeout(() => reject(new Error("Chrome never listened")), 20000);
});

const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
const ws = new WebSocket(targets.find((t) => t.type === "page").webSocketDebuggerUrl);
await new Promise((r) => ws.addEventListener("open", r, { once: true }));
let nextId = 1;
const waiting = new Map();
const thrown = [];
ws.addEventListener("message", (e) => {
  const m = JSON.parse(e.data);
  if (m.method === "Runtime.exceptionThrown") thrown.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text);
  if (m.id && waiting.has(m.id)) { waiting.get(m.id)(m); waiting.delete(m.id); }
});
const send = (method, params = {}) => new Promise((resolve) => {
  const id = nextId++;
  waiting.set(id, resolve);
  ws.send(JSON.stringify({ id, method, params }));
});

async function js(expression) {
  const r = await send("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true });
  if (r.result && r.result.exceptionDetails) throw new Error(expression.slice(0, 80) + ": " + r.result.exceptionDetails.text);
  return r.result.result.value;
}

async function until(expression, what, ms = 8000) {
  const end = Date.now() + ms;
  while (Date.now() < end) {
    try { if (await js(expression)) return true; } catch (e) {}
    await sleep(100);
  }
  check(false, what);
  return false;
}

const centre = (selector) => js(`(() => { const n = document.querySelector(${JSON.stringify(selector)}); if (!n) return null;
  const r = n.getBoundingClientRect();
  return { x: r.left + r.width / 2, y: r.top + r.height / 2, top: r.top, bottom: r.bottom }; })()`);

// `to` is where the hand goes; `settle`, when given, is measured again once
// the placeholder has opened a gap, as an eye would, and the hand goes there.
async function drag(from, to, settle) {
  const mouse = (type, x, y) => send("Input.dispatchMouseEvent", { type, x, y, button: "left", buttons: type === "mouseReleased" ? 0 : 1, clickCount: 1, pointerType: "mouse" });
  await mouse("mousePressed", from.x, from.y);
  for (let i = 1; i <= 12; i++) {
    await mouse("mouseMoved", from.x + (to.x - from.x) * i / 12, from.y + (to.y - from.y) * i / 12);
    await sleep(15);
  }
  if (settle) {
    const again = await settle();
    for (let i = 1; i <= 4; i++) { await mouse("mouseMoved", to.x + (again.x - to.x) * i / 4, to.y + (again.y - to.y) * i / 4); await sleep(15); }
    to = again;
  }
  await mouse("mouseReleased", to.x, to.y);
  await sleep(600);
}

async function cardsIn(col) {
  return (await api("GET", "/api/cards")).cards.filter((c) => c.col === col).map((c) => c.id);
}

try {
  await send("Page.enable");
  await send("Runtime.enable");
  await send("Emulation.setDeviceMetricsOverride", { width: 1500, height: 1600, deviceScaleFactor: 1, mobile: false });

  // What the page will show, made through the API.
  const made = async (title, column, key = KEY) => (await api("POST", "/api/cards", { title, column, area: "Page test" }, key)).card.id;
  const idea = await made("Page test: an idea", "ideas");
  const first = await made("Page test: agreed first", "agreed");
  const second = await made("Page test: agreed second", "agreed");
  const hand = await made("Page test: in hand", "next");
  const after = await made("Page test: after it", "next");
  await api("PUT", "/api/now", { state: "working", card: hand, step: "the page's test" }, CLAUDE);
  const task = (await api("POST", "/api/queue", { title: "Page test: a task" })).queue.find((q) => q.title === "Page test: a task").id;
  const png = Buffer.from("iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAFklEQVR4nGNkYPjPwMDAxMDAwMDAAAANHQEDK+mmyAAAAABJRU5ErkJggg==", "base64");
  await api("POST", "/api/cards/" + first + "/files?name=shot.png&caption=Page%20test", png, CLAUDE, "image/png");

  // No key: the page asks for one.
  await send("Page.navigate", { url: BASE + "/" });
  await until(`!!document.querySelector("form.gate input[type=password]")`, "without a key the page did not ask for one");

  // With one: the board.
  await js(`localStorage.setItem("kosmos-board-key", ${JSON.stringify(KEY)}); localStorage.setItem("kosmos-board-view", "board"); true`);
  await send("Page.reload");
  await until(`document.querySelectorAll(".col").length === 6`, "with a key the six columns were not drawn");
  await until(`!!document.querySelector('.list[data-col="ideas"] .cardbox[data-id="${idea}"]')`, "a card in Ideas was not in the Ideas column");
  check(await js(`document.querySelector(".strip .on")?.textContent === "Page test: in hand"`), "Claude's strip did not name the card in hand");
  const ahead = (await api("GET", "/api/queue")).queue.filter((q) => q.card !== hand);
  check(await js(`(() => { const t = document.querySelector(".strip .next")?.textContent || ""; return t.startsWith("Up next " + ${JSON.stringify(ahead[0].title)}) && !t.includes("in hand"); })()`),
    "Up next did not leave out the card in hand: " + await js(`document.querySelector(".strip .next")?.textContent`));

  // Ideas to Agreed, dropped under the last card there.
  let from = await centre(`.cardbox[data-id="${idea}"]`);
  const endOf = async (col) => { const b = await centre(`.list[data-col="${col}"] .addcard`); return { x: b.x, y: b.top - 3 }; };
  await drag(from, await endOf("agreed"), () => endOf("agreed"));
  check((await cardsIn("agreed")).at(-1) === idea, "a card dragged from Ideas to Agreed was not kept there: " + JSON.stringify(await cardsIn("agreed")));
  await until(`!!document.querySelector('.list[data-col="agreed"] .cardbox[data-id="${idea}"]')`, "the dragged card was not drawn in Agreed");

  // And back again.
  from = await centre(`.cardbox[data-id="${idea}"]`);
  await drag(from, await endOf("ideas"), () => endOf("ideas"));
  check((await cardsIn("ideas")).includes(idea), "a card dragged back from Agreed to Ideas was not kept there");

  // Above another in its own column.
  from = await centre(`.cardbox[data-id="${second}"]`);
  const target = await centre(`.cardbox[data-id="${first}"]`);
  await drag(from, { x: target.x, y: target.top + 6 });
  const agreed = await cardsIn("agreed");
  check(agreed.indexOf(second) >= 0 && agreed.indexOf(second) < agreed.indexOf(first), "a card dragged above another was not kept above it: " + JSON.stringify(agreed));

  // A card opened: its picture, fetched with the key; a note sent from it.
  await sleep(300);
  await js(`document.querySelector('.cardbox[data-id="${first}"]').click(); true`);
  await until(`(document.querySelector(".panel .file .thumb")?.style.backgroundImage || "").includes("blob:")`, "an opened card did not show its picture");
  await js(`(() => { const t = document.querySelector('.panel textarea[aria-label="Write to Claude about this card"]'); t.value = "Page test: a note"; [...document.querySelectorAll(".panel button")].find((b) => b.textContent === "Send").click(); return true; })()`);
  await sleep(800);
  const notes = (await api("GET", "/api/cards/" + first)).notes.map((n) => n.text);
  check(notes.includes("Page test: a note"), "a note sent from the card was not kept: " + JSON.stringify(notes));

  // Claude's queue: the task dragged above the card after the one in hand.
  await js(`[...document.querySelectorAll("nav.views button")].find((b) => b.textContent.startsWith("Queue")).click(); true`);
  await until(`!!document.querySelector('.qitem[data-entry="${task}"] .grip')`, "the queue did not list the task");
  const handEntry = (await api("GET", "/api/queue")).queue.find((q) => q.card === hand).id;
  check(!(await js(`!!document.querySelector('.qitem[data-entry="${handEntry}"]')`)), "the queue listed the card in hand as next");
  from = await centre(`.qitem[data-entry="${task}"] .grip`);
  const afterEntry = (await api("GET", "/api/queue")).queue.find((q) => q.card === after).id;
  const afterBox = await centre(`.qitem[data-entry="${afterEntry}"]`);
  await drag(from, { x: from.x, y: afterBox.top + 4 });
  const order = (await api("GET", "/api/queue")).queue.map((q) => q.id);
  check(order.indexOf(task) < order.indexOf(afterEntry), "a queue entry dragged above another was not kept above it: " + JSON.stringify(order));

  check(!thrown.length, "the page threw: " + JSON.stringify(thrown));
} catch (e) {
  check(false, "the page's test stopped: " + e.message);
} finally {
  try { ws.close(); } catch (e) {}
  chrome.kill();
  await sleep(300);
  rmSync(profile, { recursive: true, force: true });
}

for (const f of fails) console.log("FAIL " + f);
console.log("checks " + checks);
process.exit(fails.length ? 1 : 0);
