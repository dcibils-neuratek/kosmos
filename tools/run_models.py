#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""**Models from Hugging Face, at the prompt** (`docs/xavier.md`, *Models*).

Hugging Face is stood in for by two HTTPS servers on this Mac, reached at
10.0.2.2 with a certificate for `kosmos-test.local` signed by an authority
made for the test, which the guest's disk trusts in
`/Home/Preferences/Authorities`. The first answers the API as Hugging Face
does - a search, a repository's files with their sizes and SHA-256s - and
sends a file's address to the second with a 302, as Hugging Face sends it
to its store. The second serves the files, with Ranges, and cuts the first
download of one of them off a third of the way in.

- **`models`** with nothing there says so;
- **`models search`** finds the repository, and **`models files`** lists
  its GGUF files - the one in a quantisation the kit does not read said so,
  and the README not at all;
- **`models get`**, cut off: it says why, and `models` lists what it left
  as cut short;
- **`models get` again** asks the store for the rest with a Range, follows
  the redirect to the other port, and is whole: its SHA-256 the file's own,
  printed and checked, the file named without `.part` - and **`models
  load`** loads it, so the LLM server read a model it could run;
- **a file whose listed sum is wrong** is downloaded, refused and removed,
  and never named as a model - not even as one cut short;
- **`models rm`** of the loaded one is refused.

Usage: run_models.py IMAGE
"""

import hashlib
import json
import os
import re
import socket
import ssl
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                              # noqa: E402
import run_tls                                              # noqa: E402

REPO = "kosmos-test/tiny-phi2-GGUF"
GOOD = "tiny-phi2.F16.gguf"                 # 8.2 MB: more than two of the 4 MB pieces
WRONG = "tiny-phi2.Q8_0.gguf"               # listed with a sum that is not its own
NAME = "kosmos-test.local"


def server(work, answer):
    """An HTTPS server on this Mac; `answer(conn, head)` answers each request."""
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(os.path.join(work, "good.pem"), os.path.join(work, "server.key"))
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(8)

    def one(raw):
        try:
            raw.settimeout(60)
            conn = context.wrap_socket(raw, server_side=True)

            while True:
                head = b""

                while b"\r\n\r\n" not in head:
                    piece = conn.recv(4096)

                    if not piece:
                        conn.close()
                        return

                    head += piece

                if not answer(conn, head.decode("latin-1")):
                    conn.close()
                    return
        except (ssl.SSLError, OSError):
            raw.close()

    def loop():
        while True:
            try:
                raw, _ = listener.accept()
            except OSError:
                return

            threading.Thread(target=one, args=(raw,), daemon=True).start()

    threading.Thread(target=loop, daemon=True).start()
    return listener.getsockname()[1]


def reply(conn, status, body, kind="application/json", extra=""):
    conn.sendall(("HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n%s\r\n"
                  % (status, kind, len(body), extra)).encode() + body)
    return True


def stand_in(work, repos, found, cut=None, slow=False):
    """**Hugging Face, stood in for** by two HTTPS servers on this Mac.

    `repos` is `{repo: {listed name: (bytes, sha256)}}` - a sum that is not
    the bytes' own is a file listed wrong; `found` is what a search for
    anything answers. The API sends a file's address to the store with a
    302; the store serves it with Ranges, and cuts the first download of
    `cut` off a third of the way in. `slow` sends at about a megabyte a
    second, so a window has time to press Pause.

    Returns the API's port and what each was asked."""
    seen = {"store": [], "api": [], "cut": False}
    by_name = {}

    for repo, listing in repos.items():
        for name, (body, _) in listing.items():
            by_name[name] = body

    def store(conn, head):
        first = head.split("\r\n", 1)[0]
        seen["store"].append(head)
        m = re.match(r"GET /cas/([^? ]+)\?sig=\w+ HTTP/1\.1", first)

        if not m or m.group(1) not in by_name:
            return reply(conn, "404 Not Found", b"no such object", "text/plain")

        body = by_name[m.group(1)]
        r = re.search(r"\r\nRange: bytes=(\d+)-\r\n", head)
        start = int(r.group(1)) if r else 0
        part = body[start:]

        if r:
            conn.sendall(("HTTP/1.1 206 Partial Content\r\nContent-Type: application/octet-stream\r\n"
                          "Content-Length: %d\r\nContent-Range: bytes %d-%d/%d\r\n\r\n"
                          % (len(part), start, len(body) - 1, len(body))).encode())
        else:
            conn.sendall(("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                          "Content-Length: %d\r\n\r\n" % len(body)).encode())

        if m.group(1) == cut and not seen["cut"]:
            # A third, then gone: the connection drops as a network does.
            seen["cut"] = True
            conn.sendall(body[:len(body) // 3])
            return False

        step = 128 * 1024 if slow else len(part)

        for at in range(0, len(part), max(1, step)):
            conn.sendall(part[at:at + step])
            if slow:
                time.sleep(0.12)

        return True

    store_port = server(work, store)

    def api(conn, head):
        first = head.split("\r\n", 1)[0]
        seen["api"].append(first)

        if first.startswith("GET /api/models?search="):
            return reply(conn, "200 OK", json.dumps(found).encode())

        for repo, listing in repos.items():
            if first.startswith("GET /api/models/%s/tree/main " % repo):
                tree = [{"type": "file", "path": "README.md", "size": 20}]

                for name, (body, sha) in sorted(listing.items()):
                    tree.append({"type": "file", "path": name, "size": len(body),
                                 "lfs": {"oid": sha, "size": len(body)}})

                return reply(conn, "200 OK", json.dumps(tree).encode())

            m = re.match(r"GET /%s/resolve/main/([^ ]+) HTTP" % re.escape(repo), first)

            if m:
                return reply(conn, "302 Found", b"", "text/plain",
                             "Location: https://10.0.2.2:%d/cas/%s?sig=abc123\r\n"
                             % (store_port, m.group(1)))

        return reply(conn, "404 Not Found", b"{}")

    return server(work, api), seen


def authority(work):
    """The test's authority in DER, for the guest's `/Home/Preferences/Authorities`."""
    der = os.path.join(work, "test.der")
    subprocess.run(["openssl", "x509", "-in", os.path.join(work, "ca.pem"), "-outform", "DER",
                    "-out", der], check=True, capture_output=True)
    return der


def sha(body):
    return hashlib.sha256(body).hexdigest()


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
    models_dir = os.path.join(ROOT, "build", "models")

    if not os.path.exists(os.path.join(models_dir, GOOD)):
        print("SKIP: no tiny phi-2 in build/models - `make llm-tiny`, docs/xavier.md X3")
        return 0

    work = scratch.directory("models")
    run_tls.pki(work, NAME)

    files = {}

    for f in (GOOD, WRONG, "tiny-phi2.Q4_0.gguf"):
        with open(os.path.join(models_dir, f), "rb") as fh:
            files[f] = fh.read()

    # The Q4_0 is listed as Q4_K_M, a quantisation the kit does not read;
    # the Q8_0 with a sum that is not its own.
    sums = {GOOD: sha(files[GOOD]), WRONG: "0" * 64}
    api_port, seen = stand_in(work, {REPO: {
        GOOD: (files[GOOD], sums[GOOD]),
        WRONG: (files[WRONG], sums[WRONG]),
        "tiny-phi2.Q4_K_M.gguf": (files["tiny-phi2.Q4_0.gguf"], sha(files["tiny-phi2.Q4_0.gguf"])),
    }}, [{"id": REPO, "downloads": 42, "likes": 1, "gguf": {"architecture": "phi2"}},
         {"id": "kosmos-test/other-GGUF", "downloads": 7, "likes": 0,
          "gguf": {"architecture": "llama"}}], cut=GOOD)

    with open(os.path.join(work, "README"), "w") as f:
        f.write("models are put here\n")

    disk = os.path.join(work, "disk.img")
    subprocess.run([os.path.join(ROOT, "build", "host", "lua"), os.path.join(HERE, "kfs.lua"),
                    "create", disk, "64",
                    authority(work) + ":/Home/Preferences/Authorities/test.der",
                    os.path.join(work, "README") + ":/Home/Models/README"],
                   check=True, capture_output=True, cwd=ROOT)

    os.environ["KOSMOS_DISK"] = disk
    import run_screenshot as R                              # noqa: E402

    board = "X86_ARGS" if R.machine(image) == "x86_64" else "QEMU_ARGS"
    saved = getattr(R, board)
    setattr(R, board, saved + ["-netdev", "user,id=net0",
                               "-device", R.device(image, "net") + ",netdev=net0"])

    try:
        guest = R.Guest(image, 240)
    finally:
        setattr(R, board, saved)

    src = " --source https://10.0.2.2:%d --name %s" % (api_port, NAME)
    said = {}

    def run(key, line):
        mark = len(guest.seen)
        guest.type(line)
        guest.wait_for_line(") ended, code", "answered " + line, since=mark)
        said[key] = guest.seen[mark:].replace("\r", "")

    error = None

    try:
        guest.wait_for(R.PROMPT, "the prompt")
        guest.wait_for("net: an address from DHCP", "a lease")
        run("empty", "models")
        run("search", "models search tiny" + src)
        run("files", "models files " + REPO + src)
        run("cut", "models get %s %s%s" % (REPO, GOOD, src))
        run("left", "models")
        run("resumed", "models get %s %s%s" % (REPO, GOOD, src))
        run("load", "models load " + GOOD)
        run("wrong", "models get %s %s%s" % (REPO, WRONG, src))
        run("after", "models")
        run("rm", "models rm " + GOOD)
    except Exception as e:                  # noqa: BLE001 - said below
        error = "%s: %s" % (type(e).__name__, str(e).splitlines()[0])
    finally:
        guest.close()

    fails, checks = [], 0

    def check(ok, what):
        nonlocal checks
        checks += 1
        if not ok:
            fails.append(what)

    check(error is None, "the machine stopped: %s" % error)
    g = lambda k: said.get(k, "")                           # noqa: E731

    check("no models in /Home/Models" in g("empty"), "an empty folder was not said: %r" % g("empty"))
    check(re.search(r"%s\s+42 downloads\s+phi2" % re.escape(REPO), g("search"))
          and re.search(r"kosmos-test/other-GGUF\s+7 downloads\s+llama\s+a llama model: the LLM Kit "
                        r"has a forward pass for phi2, qwen2 only", g("search"))
          and "expand%5B%5D=gguf" in seen["api"][0],
          "the search did not list the repositories: %r" % g("search"))
    check(re.search(r"%s\s+8\.2 MB\s+F16" % re.escape(GOOD), g("files"))
          and re.search(r"tiny-phi2\.Q4_K_M\.gguf\s+\S+ MB\s+Q4_K_M\s+the LLM Kit reads Q4_0, Q8_0, "
                        r"F16 and F32, not Q4_K_M", g("files"))
          and "README" not in g("files"),
          "the files were not listed with their kinds, the unreadable one said so: %r" % g("files"))
    check(seen["cut"] and "models: " in g("cut") and "checked" not in g("cut"),
          "the cut download did not say why: %r" % g("cut"))
    check(re.search(r"%s\.part\s+.*cut short" % re.escape(GOOD), g("left")),
          "what the cut download left was not listed as cut short: %r" % g("left"))

    ranges = [re.search(r"Range: bytes=(\d+)-", h) for h in seen["store"]]
    asked_from = [int(r.group(1)) for r in ranges if r]
    check(asked_from and 0 < asked_from[0] <= len(files[GOOD]) // 3,
          "the second get did not ask for the rest with a Range: %r" % asked_from)
    check(("models: /Home/Models/%s, 8.2 MB, SHA-256 %s checked" % (GOOD, sums[GOOD])) in g("resumed"),
          "the resumed download was not whole and checked: %r" % g("resumed"))
    check(re.search(r"models: %s is loaded" % re.escape(GOOD), g("load")),
          "the downloaded model would not load: %r" % g("load"))
    check(re.search(r"SHA-256 is [0-9a-f]{64}, not the 0{64} it should be - removed", g("wrong")),
          "a file with the wrong sum was not refused for it: %r" % g("wrong"))
    check(re.search(r"%s\s+8\.2 MB\s+F16\s+loaded" % re.escape(GOOD), g("after"))
          and WRONG not in g("after"),
          "after it all, the folder is not the loaded model and no wrong one: %r" % g("after"))
    check("is loaded; load another first" in g("rm"), "removing the loaded model was not refused: %r" % g("rm"))

    if fails:
        for f in fails:
            print("FAIL: " + f)
        print("the API was asked: %r" % seen["api"])
        return 1

    print("PASS: %d checks on models - a search and a repository's files from a stand-in Hugging "
          "Face over HTTPS, a download cut off and resumed from %d with a Range through a "
          "redirect to another port, whole and held to its SHA-256 and loaded, a wrong sum "
          "refused" % (checks, asked_from[0]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
