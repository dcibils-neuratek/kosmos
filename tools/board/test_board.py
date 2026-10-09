#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The Kosmos Board's API, on this Mac: `wrangler dev --local` over a
database made fresh in a scratch folder, and every route asked.

    python3 tools/board/test_board.py

A test key is made for the run (the local exception: it never leaves this
machine's scratch folder). Checked: nothing is answered without a key, or
with a withdrawn one; a card made, read, edited, moved into a column and
before another, noted and removed; ordering in a column; decisions asked
and answered, the answer a note on its card; what changed since a time,
signed by the key's name; the Markdown export; and bad bodies refused
with a reason.
"""

import json
import os
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import scratch                                              # noqa: E402

fails, checks = [], 0


def check(ok, what):
    global checks
    checks += 1
    if not ok:
        fails.append(what)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def main():
    state = scratch.directory("board")
    run = lambda *a: subprocess.run(["wrangler", *a, "--local", "--persist-to", state],
                                    cwd=HERE, capture_output=True, text=True)
    made = run("d1", "execute", "kosmos-board", "--file", "schema.sql")
    if made.returncode != 0:
        print("FAIL: the schema would not apply: " + made.stderr[-600:])
        return 1

    keyfile = os.path.join(state, "test.key")
    other = os.path.join(state, "other.key")
    for name, path in (("Test", keyfile), ("Other", other)):
        k = subprocess.run([sys.executable, "keys.py", "--local", "--persist-to", state, "make", name, "--to", path],
                           cwd=HERE, capture_output=True, text=True)
        if k.returncode != 0:
            print("FAIL: a key would not be made: " + (k.stderr or k.stdout)[-600:])
            return 1

    key = open(keyfile).read().strip()
    port = free_port()
    dev = subprocess.Popen(["wrangler", "dev", "--local", "--persist-to", state, "--port", str(port), "--ip", "127.0.0.1"],
                           cwd=HERE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    base = "http://127.0.0.1:%d" % port

    def call(method, path, body=None, auth=key):
        req = urllib.request.Request(base + path, method=method,
                                     data=None if body is None else (body if isinstance(body, bytes) else json.dumps(body).encode()))
        if auth:
            req.add_header("Authorization", "Bearer " + auth)
        if body is not None:
            req.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(req, timeout=20) as r:
                raw = r.read().decode()
                return r.status, (json.loads(raw) if r.headers.get("content-type", "").startswith("application/json") else raw)
        except urllib.error.HTTPError as e:
            raw = e.read().decode()
            try:
                return e.code, json.loads(raw)
            except ValueError:
                return e.code, raw

    try:
        deadline = time.time() + 60
        while True:
            try:
                urllib.request.urlopen(base + "/", timeout=2)
                break
            except Exception:                                   # noqa: BLE001 - not up yet
                if time.time() > deadline or dev.poll() is not None:
                    print("FAIL: wrangler dev never answered")
                    return 1
                time.sleep(0.5)

        s, b = call("GET", "/api/cards", auth=None)
        check(s == 401 and "key" in b.get("error", ""), "a request with no key was answered: %r" % ((s, b),))
        s, _ = call("GET", "/api/cards", auth="kb_" + "0" * 40)
        check(s == 401, "an unknown key was answered")
        s, b = call("GET", "/api/me")
        check(s == 200 and b.get("name") == "Test", "the key's name: %r" % ((s, b),))

        s, b = call("POST", "/api/cards", {"title": "Mail M6: write, reply and forward", "column": "next", "area": "Mail", "ref": "Mail M6"})
        check(s == 201 and b["card"]["col"] == "next" and b["card"]["in_roadmap"] is False, "a card made: %r" % ((s, b),))
        m6 = b["card"]["id"]
        s, b = call("POST", "/api/cards", {"title": "Astra split D2", "column": "next", "area": "Desktop"})
        d2 = b["card"]["id"]
        s, b = call("POST", "/api/cards", {"title": "Read NTFS drives"})
        ntfs = b["card"]["id"]
        check(b["card"]["col"] == "ideas", "a card with no column is not an idea: %r" % b)

        s, b = call("GET", "/api/cards?column=next")
        check([c["id"] for c in b["cards"]] == [m6, d2], "Next in the order made: %r" % b)

        s, b = call("POST", "/api/cards/%s/move" % d2, {"column": "next", "before": m6})
        s, b = call("GET", "/api/cards?column=next")
        check([c["id"] for c in b["cards"]] == [d2, m6], "a card moved before another: %r" % b)

        s, b = call("POST", "/api/cards/%s/move" % ntfs, {"column": "agreed"})
        check(s == 200 and b["card"]["col"] == "agreed", "a card moved to a column: %r" % ((s, b),))

        s, b = call("PATCH", "/api/cards/%s" % m6, {"detail": "The composer as drawn.", "in_roadmap": True})
        check(s == 200 and b["card"]["detail"] == "The composer as drawn." and b["card"]["in_roadmap"] is True,
              "a card edited: %r" % ((s, b),))

        s, b = call("POST", "/api/cards/%s/notes" % m6, {"text": "M5 built in 0.11.91."})
        check(s == 201 and b["note"]["author"] == "Test", "a note signed: %r" % ((s, b),))

        s, b = call("POST", "/api/decisions", {"card": d2, "question": "Who draws an ordinary window?",
                                               "options": ["Every window itself", "The server"]})
        check(s == 201 and b["decision"]["answer"] is None, "a decision asked: %r" % ((s, b),))
        did = b["decision"]["id"]
        s, b = call("GET", "/api/decisions?open=true")
        check(len(b["decisions"]) == 1, "the open decisions: %r" % b)
        s, b = call("PATCH", "/api/decisions/%d" % did, {"answer": "Every window itself", "words": "as recommended"})
        check(s == 200 and b["decision"]["answer"] == "Every window itself", "a decision answered: %r" % ((s, b),))
        s, b = call("GET", "/api/decisions?open=true")
        check(len(b["decisions"]) == 0, "an answered decision still open")

        s, b = call("GET", "/api/cards/%s" % d2)
        check(any("Decided: Who draws" in n["text"] for n in b["notes"]), "the answer is not a note on its card: %r" % b)
        check([h["kind"] for h in b["history"]][:2] == ["card.new", "card.move"], "a card's history: %r" % b["history"])

        s, b = call("GET", "/api/changes?since=1970-01-01T00:00:00Z")
        kinds = [c["kind"] for c in b["changes"]]
        check(s == 200 and kinds.count("card.new") == 3 and "decision.answer" in kinds and all(c["author"] == "Test" for c in b["changes"]),
              "what changed, signed: %r" % kinds)
        later = b["now"]
        s, b = call("GET", "/api/changes?since=" + later)
        check(b["changes"] == [], "changes since the last look were not empty")

        s, b = call("DELETE", "/api/cards/%s" % ntfs)
        s, b = call("GET", "/api/cards")
        check(ntfs not in [c["id"] for c in b["cards"]], "a removed card is still listed")

        # What Claude is doing now: idle until said, then its card, step and
        # wait; `since` held while the card is the same, moved with the card.
        s, b = call("GET", "/api/now")
        check(s == 200 and b["now"]["state"] == "idle" and b["now"]["card"] is None, "now, before anything: %r" % b)
        s, b = call("PUT", "/api/now", {"card": m6, "step": "the composer's To field", "state": "working"})
        check(s == 200 and b["now"]["card"] == m6 and b["now"]["title"] == "Mail M6: write, reply and forward"
              and b["now"]["author"] == "Test", "now, said: %r" % ((s, b),))
        first_since = b["now"]["since"]
        time.sleep(1.1)
        s, b = call("PUT", "/api/now", {"card": m6, "step": "running x86-mail-3", "state": "waiting", "waiting": "x86-mail-3"})
        check(b["now"]["since"] == first_since and b["now"]["waiting"] == "x86-mail-3" and b["now"]["updated"] > first_since,
              "the same card kept its since: %r" % b)
        s, b = call("PUT", "/api/now", {"card": d2, "step": "D2", "state": "working"})
        check(b["now"]["since"] > first_since, "a new card did not move since: %r" % b)
        s, b = call("PUT", "/api/now", {"state": "sleeping"})
        check(s == 400 and "state" in b["error"], "an unknown state taken: %r" % ((s, b),))
        s, b = call("GET", "/api/changes?since=" + later)
        check([c["kind"] for c in b["changes"]].count("now") == 3, "now's words not in the changes: %r" % b)

        s, md = call("GET", "/api/export.md")
        check(s == 200 and "## Next (2)" in md and "**Astra split D2**" in md and "not yet in roadmap.md" in md,
              "the Markdown export: %r" % md[:300])

        s, b = call("POST", "/api/cards", {"column": "next"})
        check(s == 400 and "title" in b["error"], "a card with no title made: %r" % ((s, b),))
        s, b = call("POST", "/api/cards", {"title": "x", "column": "someday"})
        check(s == 400 and "column" in b["error"], "an unknown column taken: %r" % ((s, b),))
        s, b = call("POST", "/api/cards", b"not json")
        check(s == 400, "a body that is not JSON taken")
        s, b = call("POST", "/api/cards", {"title": "x" * 300})
        check(s == 400 and "longer" in b["error"], "a title too long taken")

        subprocess.run([sys.executable, "keys.py", "--local", "--persist-to", state, "revoke", "Other"], cwd=HERE, capture_output=True)
        s, _ = call("GET", "/api/me", auth=open(other).read().strip())
        check(s == 401, "a withdrawn key still answered")
        s, _ = call("GET", "/api/me")
        check(s == 200, "withdrawing one key withdrew another")
    finally:
        dev.terminate()
        try:
            dev.wait(10)
        except subprocess.TimeoutExpired:
            dev.kill()

    if fails:
        print("FAIL: %d of %d checks on the Kosmos Board's API:" % (len(fails), checks))
        for f in fails:
            print("  " + f)
        return 1

    print("PASS: %d checks on the Kosmos Board's API (no key and a withdrawn key refused; cards made, "
          "ordered, moved, edited, noted and removed; decisions asked and answered onto their card; "
          "changes since a time, signed; what Claude is doing now, its since held per card; the Markdown "
          "export; bad bodies refused)." % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
