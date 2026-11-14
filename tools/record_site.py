#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""**Record a real site for WebCore to lay out on Kosmos** - JetSki W6d's
recorded sites (`docs/w6.md`), not a test.

Usage: record_site.py NAME URL [SITES]

What is recorded is **the page as a desktop browser left it**: this Mac's
Chrome, headless, opens the address at 1280 pixels wide, runs its scripts
for fifteen seconds of virtual time and gives back the document they made
(`--dump-dom`). Until W7 Kosmos has no network to run a site's scripts
against, and YouTube's served page is nothing but scripts - so the scripts
are taken out and what they built stays. Every style sheet, every `url()`
in one and in a `style`, and every picture is fetched once and the
reference made a local file. The same Chrome takes a picture of the page
at the same size, so what WebCore paints has a reference beside it.

A recording is a folder: `page.html`, `files/`, `chrome.png` and
`record.json` - the address, the time, Chrome's version, and every file
with the address it came from, its type and its SHA-256. It lives under
`~/Kosmos/sites/NAME` (Diego, 14 November 2026), and never in the
repository: a site's bytes are its authors'.

`tools/run_site.py` paints it.
"""

import datetime
import hashlib
import html
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import scratch                                               # noqa: E402

CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
WIDTH, HEIGHT = 1280, 1600
UA = ("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/157.0.0.0 Safari/537.36")


def chrome(profile, *args):
    """Chrome headless at the page's size. Virtual time lets a page's timers
    finish; a page whose timers never do (CNN's) holds it for ever, and is
    given twenty seconds of real time instead."""
    base = [CHROME, "--headless", "--disable-gpu", "--hide-scrollbars",
            "--user-data-dir=" + profile, "--window-size=%d,%d" % (WIDTH, HEIGHT)]
    try:
        return subprocess.run(base + ["--virtual-time-budget=15000", *args],
                              capture_output=True, timeout=90), "virtual 15 s"
    except subprocess.TimeoutExpired:
        return subprocess.run(base + ["--timeout=20000", *args],
                              capture_output=True, timeout=120), "real 20 s"


class Recording:
    def __init__(self, out, base):
        self.out, self.base, self.files, self.known = out, base, [], {}

    def fetch(self, url, kind):
        """The local name of url, relative to the page's folder; None if it
        could not be had."""
        url = html.unescape(url).strip()
        if url.startswith("//"):
            url = "https:" + url
        if not url.startswith(("http://", "https://")):
            return None
        if url in self.known:
            return self.known[url]
        self.known[url] = None
        try:
            # Asked for as Chrome asks, AVIF first: a recording holds what
            # the site serves a current browser, and what Kosmos cannot
            # decode yet shows as missing rather than being steered round
            # (Diego, 14 November 2026: "we need to have all decoders").
            req = urllib.request.Request(url, headers={
                "User-Agent": UA, "Referer": self.base,
                "Accept": "image/avif,image/webp,image/apng,image/svg+xml,image/*,*/*;q=0.8"
                if kind == "file" else "text/css,*/*;q=0.1"})
            with urllib.request.urlopen(req, timeout=30) as r:
                data, ctype = r.read(), r.headers.get("Content-Type", "").split(";")[0].strip()
        except Exception as e:                               # noqa: BLE001
            print("missed", url[:120], e)
            return None
        ext = os.path.splitext(urllib.parse.urlparse(url).path)[1].lower()
        if kind == "css" or ctype == "text/css":
            ext = ".css"
        elif ext not in (".svg", ".png", ".jpg", ".jpeg", ".webp", ".gif", ".avif",
                         ".woff", ".woff2", ".ttf", ".otf"):
            ext = {"image/svg+xml": ".svg", "image/png": ".png", "image/jpeg": ".jpg",
                   "image/webp": ".webp", "image/gif": ".gif", "image/avif": ".avif",
                   "font/woff2": ".woff2", "font/woff": ".woff"}.get(ctype, ".bin")
        name = "files/" + hashlib.sha1(url.encode()).hexdigest()[:12] + ext
        self.known[url] = name
        if ext == ".css":
            # A sheet sits in files/, beside what it names.
            data = self.urls(data.decode("utf-8", "replace"), url, inside=True).encode()
        with open(os.path.join(self.out, name), "wb") as f:
            f.write(data)
        self.files.append({"url": url, "file": name, "type": ctype, "bytes": len(data),
                           "sha256": hashlib.sha256(data).hexdigest()})
        return name

    def urls(self, text, base, inside=False):
        """Every url() in a sheet or a style made a local file."""
        def one(m):
            ref = m.group(2)
            if ref.startswith(("data:", "#")):
                return m.group(0)
            got = self.fetch(urllib.parse.urljoin(base, html.unescape(ref)), "file")
            if not got:
                return m.group(0)
            return "url(%s%s%s)" % (m.group(1), os.path.basename(got) if inside else got, m.group(1))
        text = re.sub(r"url\(\s*(['\"]?)([^'\")]+)\1\s*\)", one, text)
        # @import "x.css" without url().
        return re.sub(r"@import\s+(['\"])([^'\"]+)\1",
                      lambda m: '@import "%s"' % (os.path.basename(g) if inside else g)
                      if (g := self.fetch(urllib.parse.urljoin(base, m.group(2)), "css")) else m.group(0),
                      text)


def attribute(tag, name):
    m = re.search(r'\s%s="([^"]*)"' % name, tag)
    return m and html.unescape(m.group(1))


def first_candidate(srcset):
    return srcset.split(",")[0].strip().split(" ")[0] if srcset else None


def record(name, url, sites):
    out = os.path.join(sites, name)
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(os.path.join(out, "files"))
    # A fresh Chrome profile each time, so nothing a site stored before -
    # a cookie, a consent - shapes the page; gone when the tool ends.
    profile = scratch.directory("chrome")
    dumped, waited = chrome(profile, "--dump-dom", url)
    if dumped.returncode != 0 or not dumped.stdout:
        sys.exit("record: Chrome gave no document for %s\n%s" % (url, dumped.stderr.decode()[-800:]))
    chrome(profile, "--screenshot=" + os.path.join(out, "chrome.png"), url)
    page = dumped.stdout.decode("utf-8", "replace")
    rec = Recording(out, url)

    # What the scripts made stays; the scripts go, and what would reach out
    # for more of the page while it is laid out goes with them.
    page = re.sub(r"<script\b[^>]*>.*?</script>", "", page, flags=re.S | re.I)
    page = re.sub(r"<base\b[^>]*>", "", page, flags=re.I)
    page = re.sub(r"<iframe\b[^>]*>.*?</iframe>", "", page, flags=re.S | re.I)
    page = re.sub(r"<source\b[^>]*>", "", page, flags=re.I)

    def link(m):
        tag = m.group(0)
        rel = (attribute(tag, "rel") or "").lower()
        href = attribute(tag, "href")
        if "stylesheet" not in rel or not href:
            return "" if rel in ("preload", "prefetch", "preconnect", "dns-prefetch", "modulepreload") else tag
        got = rec.fetch(urllib.parse.urljoin(url, href), "css")
        return re.sub(r'\shref="[^"]*"', ' href="%s"' % got, tag) if got else ""

    page = re.sub(r"<link\b[^>]*>", link, page, flags=re.I)

    def img(m):
        tag = m.group(0)
        src = attribute(tag, "src")
        if not src or src.startswith("data:"):
            src = first_candidate(attribute(tag, "srcset")) or src
        tag = re.sub(r'\s(srcset|sizes|loading|decoding)="[^"]*"', "", tag)
        if src and not src.startswith("data:"):
            got = rec.fetch(urllib.parse.urljoin(url, src), "file")
            tag = re.sub(r'\ssrc="[^"]*"', "", tag)
            tag = tag[:4] + ' src="%s"' % (got or "") + tag[4:]
        return tag

    page = re.sub(r"<img\b[^>]*>", img, page, flags=re.I)
    page = re.sub(r"<style\b[^>]*>.*?</style>", lambda m: rec.urls(m.group(0), url), page,
                  flags=re.S | re.I)
    page = re.sub(r'\sstyle="([^"]*url\([^"]*)"',
                  lambda m: ' style="%s"' % html.escape(rec.urls(html.unescape(m.group(1)), url), quote=True),
                  page)

    # Chrome's document is UTF-8 and says so nowhere once its scripts are
    # gone, and a page that does not say is read as Latin-1.
    page = re.sub(r"<meta\s+charset=[^>]*>", "", page, flags=re.I)
    page = re.sub(r"<head\b[^>]*>", lambda m: m.group(0) + '<meta charset="utf-8">', page, count=1, flags=re.I)
    with open(os.path.join(out, "page.html"), "w", encoding="utf-8") as f:
        f.write(page)
    version = subprocess.run([CHROME, "--version"], capture_output=True, text=True).stdout.strip()
    with open(os.path.join(out, "record.json"), "w") as f:
        json.dump({"name": name, "url": url, "width": WIDTH, "height": HEIGHT,
                   "recorded": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
                   "by": version, "waited": waited, "page_bytes": len(page.encode()), "files": rec.files}, f, indent=1)
    total = sum(x["bytes"] for x in rec.files)
    print("record: %s - page %d bytes, %d files, %d bytes, in %s"
          % (name, len(page.encode()), len(rec.files), total, out))


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    record(sys.argv[1], sys.argv[2],
           sys.argv[3] if len(sys.argv) > 3 else os.path.expanduser("~/Kosmos/sites"))
