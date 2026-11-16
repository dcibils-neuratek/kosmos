#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""**The top 20: JetSki's benchmark** (`roadmap.md`; Diego, 16 November
2026: "This list is our benchmark for our Browser").

Twenty-three sites that show a lot of content without signing in - most of
the twenty most visited in the United States, and news - each opened in a fresh
JetSki on a Kosmos machine, and for each:

- **how it ended**: loaded, could not load, its page's process stopped, or
  still loading after the time allowed;
- **how long**, from `jetski: loading` to `jetski: loaded` in the machine's
  own log, so the clock is the machine's and not the network's to this Mac;
- **what Privacy and Ad Blocking kept out** (`webkit: view N kept out`),
  once it is on;
- **the cookies it left**, read from `cookies.db` before and after: its
  own, and other sites';
- **a picture** of the screen, over VNC when the machine shares it.

Beside each, this Mac's Chrome, headless at the same size: a picture and a
time (a whole process's, start to screenshot, so a second or so above the
page's own).

**Five seconds is the standard** (Diego, 16 November 2026: "every
website that we load can't take more than 5 seconds. That's a standard. If
it takes more than 5 seconds, the browser is not behaving as expected"). A
site that has not loaded within `STANDARD` seconds is a failure of the
browser's, named at the end, and the run exits 1 - not a site being slow.

Not in the gate: the sites are live, and change by the hour. The results
are kept by version in `bench/top20/<version>-<machine>.json`, and
`build/top20/<run>/report.html` shows a run beside the one before it.

Usage:
  top20.py [--machine ADDRESS] [--only google,youtube] [--wait 90] [--no-chrome]

The machine is the M700 by default, 192.168.1.35, reached by Telnet
(`kosmos_telnet.py`) - real times, where QEMU's are not
(`CLAUDE.md`, *QEMU numbers are not performance numbers*).
"""

import datetime
import html
import json
import os
import re
import sqlite3
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import kosmos_telnet                                         # noqa: E402

# The most a site may take, from `jetski: loading` to `jetski: loaded`.
STANDARD = 5.0

# Diego's list, 16 November 2026, in its order - and the same day, the
# seven that show a sign-in page rather than their content replaced: "a
# lot of the wbsites require login to show all the real content", "we will
# need to find other websites like news.google.com", a site kept only if it
# "shows a lot of content without login". Then infobae.com,
# elobservador.com.uy and elpais.com.uy added.
SITES = [
    (1, "Google", "google.com"),
    (2, "YouTube", "youtube.com"),
    (3, "Google News", "news.google.com"),
    (4, "Amazon", "amazon.com"),
    (5, "Reddit", "reddit.com"),
    (6, "Yahoo!", "yahoo.com"),
    (7, "Microsoft Bing", "bing.com"),
    (8, "IMDb", "imdb.com"),
    (9, "CNN", "cnn.com"),
    (10, "Apple", "apple.com"),
    (11, "The Guardian", "theguardian.com"),
    (12, "Wikipedia", "wikipedia.org"),
    (13, "BBC", "bbc.com"),
    (14, "ESPN", "espn.com"),
    (15, "Walmart", "walmart.com"),
    (16, "TikTok", "tiktok.com"),
    (17, "eBay", "ebay.com"),
    (18, "The Weather Channel", "weather.com"),
    (19, "The New York Times", "nytimes.com"),
    (20, "Temu", "temu.com"),
    (21, "Infobae", "infobae.com"),
    (22, "El Observador", "elobservador.com.uy"),
    (23, "El País (Uruguay)", "elpais.com.uy"),
]

COOKIES = "/Home/Settings/JetSki/cookies.db"
WINDOW = (1200, 800)
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
STAMP = re.compile(r"^\[(\d+\.\d+)\] (.*)$")


def arguments(argv):
    chosen = {"machine": "192.168.1.35", "only": None, "wait": 90.0, "chrome": True}
    at = 0
    while at < len(argv):
        word = argv[at]
        if word == "--machine":
            chosen["machine"] = argv[at + 1]; at += 1
        elif word == "--only":
            chosen["only"] = set(argv[at + 1].split(",")); at += 1
        elif word == "--wait":
            chosen["wait"] = float(argv[at + 1]); at += 1
        elif word == "--no-chrome":
            chosen["chrome"] = False
        else:
            sys.exit(__doc__)
        at += 1
    return chosen


def version():
    with open(os.path.join(ROOT, "VERSION")) as f:
        return f.read().strip()


def log_lines(session, count=120):
    """The machine's last lines, as (seconds, text)."""
    said = session.run("log %d" % count).decode(errors="replace")
    lines = []
    for line in said.splitlines():
        found = STAMP.match(line.strip())
        if found:
            lines.append((float(found.group(1)), found.group(2)))
    return lines


def cookies_of(session, out):
    """Every cookie's site in the machine's `cookies.db`, as a set of
    (domain, name, path)."""
    try:
        data = session.get(COOKIES)
    except IOError:
        return set()
    with open(out, "wb") as f:
        f.write(data)
    try:
        db = sqlite3.connect(out)
        rows = set(db.execute("select domain, name, path from Cookie"))
        db.close()
        return rows
    except sqlite3.Error:
        return set()


def own(domain, site):
    """Whether a cookie's domain is the site's own: the site, or a name
    under it. `cloud.microsoft` and the rest are taken as given."""
    domain = domain.lstrip(".")
    return domain == site or domain.endswith("." + site)


def visit(session, site, wait):
    """One site, in a fresh JetSki: how it ended, and when."""
    session.run("kill jetski")
    time.sleep(1)
    started = log_lines(session, 5)
    after = started[-1][0] if started else 0.0
    session.run("open jetski https://%s/" % site)

    result = {"ended": "still loading", "seconds": None, "kept_out": 0, "cookies_kept_back": 0,
              "address": None}
    loading_at = None
    deadline = time.monotonic() + wait + 10
    while time.monotonic() < deadline:
        time.sleep(2)
        for when, text in log_lines(session):
            if when <= after:
                continue
            if loading_at is None and text.startswith("jetski: loading "):
                loading_at = when
            found = re.match(r"webkit: view \d+ kept out (\d+) requests, cookies of (\d+)", text)
            if found:
                result["kept_out"] = max(result["kept_out"], int(found.group(1)))
                result["cookies_kept_back"] = max(result["cookies_kept_back"], int(found.group(2)))
            if loading_at is None:
                continue
            ended = None
            if text.startswith("jetski: loaded "):
                ended = "loaded"
            elif text.startswith("jetski: could not load"):
                ended = "could not load"
            elif "the page's process ended" in text:
                ended = "the page's process stopped"
            if ended and result["seconds"] is None:
                result["ended"] = ended
                result["seconds"] = round(when - loading_at, 1)
                result["address"] = text.split(" ", 2)[-1]
        if result["seconds"] is not None:
            break
        if loading_at is not None and time.monotonic() > deadline - 10:
            break
    if result["seconds"] is None and loading_at is not None:
        result["seconds"] = wait
    # What arrived after the load: a page goes on asking for a while.
    time.sleep(3)
    for when, text in log_lines(session, 60):
        found = re.match(r"webkit: view \d+ kept out (\d+) requests, cookies of (\d+)", text)
        if found and when > after:
            result["kept_out"] = max(result["kept_out"], int(found.group(1)))
            result["cookies_kept_back"] = max(result["cookies_kept_back"], int(found.group(2)))
    return result


def screen(machine, out):
    """The machine's screen over VNC, when it shares it."""
    try:
        done = subprocess.run([sys.executable, os.path.join(HERE, "kosmos_vnc.py"), machine.split(":")[0],
                               "shot", out], capture_output=True, timeout=60)
        return done.returncode == 0 and os.path.exists(out)
    except subprocess.TimeoutExpired:
        return False


def chrome(site, out):
    """This Mac's Chrome, headless, at the window's size: a picture and the
    seconds the whole of it took."""
    if not os.path.exists(CHROME):
        return None
    profile = os.path.join(os.path.dirname(out), "chrome-profile")
    start = time.monotonic()
    try:
        subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--hide-scrollbars",
                        "--user-data-dir=" + profile, "--window-size=%d,%d" % WINDOW,
                        "--screenshot=" + out, "https://%s/" % site],
                       capture_output=True, timeout=120)
    except subprocess.TimeoutExpired:
        return None
    return round(time.monotonic() - start, 1) if os.path.exists(out) else None


def previous(machine_name, now_file):
    """The run before this one on the same machine, if any."""
    folder = os.path.join(ROOT, "bench", "top20")
    if not os.path.isdir(folder):
        return None
    runs = sorted(f for f in os.listdir(folder)
                  if f.endswith("-%s.json" % machine_name) and f != os.path.basename(now_file))
    if not runs:
        return None
    with open(os.path.join(folder, runs[-1])) as f:
        return json.load(f)


def report(run, before, out_dir):
    rows = []
    earlier = {s["site"]: s for s in before["sites"]} if before else {}
    for s in run["sites"]:
        was = earlier.get(s["site"])
        then = ""
        if was and was.get("seconds") is not None and s.get("seconds") is not None:
            then = "%+.1f s" % (s["seconds"] - was["seconds"])
        pictures = "".join('<img src="%s" alt="">' % html.escape(p)
                           for p in (s.get("picture"), s.get("chrome_picture")) if p)
        rows.append("<tr class='%s'><td>%d</td><td><b>%s</b><br><span>%s</span></td><td>%s</td>"
                    "<td class=n>%s</td><td class=n>%s</td><td class=n>%s</td><td class=n>%s</td>"
                    "<td class=n>%s</td><td class=n>%s</td></tr><tr><td></td><td colspan=8 class=pics>%s</td></tr>"
                    % ("good" if s["ended"] == "loaded" else "bad", s["rank"], html.escape(s["name"]),
                       html.escape(s["site"]), html.escape(s["ended"]),
                       "%.1f s" % s["seconds"] if s.get("seconds") is not None else "-", then,
                       "%.1f s" % s["chrome_seconds"] if s.get("chrome_seconds") else "-",
                       s.get("kept_out", 0), s.get("cookies_own", 0), s.get("cookies_others", 0),
                       pictures))
    loaded = sum(1 for s in run["sites"] if s["ended"] == "loaded")
    page = """<!doctype html>
<!-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. -->
<html lang="en"><head><meta charset="utf-8"><title>Top 20 - %(version)s</title>
<style>
:root { --ink: #18202c; --dim: #5a6474; --rule: #dde2ea; --good: #2e9b5f; --bad: #c2410c; --ground: #f6f7f9; }
body { margin: 0; padding: 32px 24px; background: var(--ground); color: var(--ink); font: 14px/1.5 "IBM Plex Sans", system-ui, sans-serif; }
h1 { margin: 0 0 4px; font-size: 26px; } p { color: var(--dim); margin: 0 0 18px; }
table { border-collapse: collapse; width: 100%%; max-width: 1200px; background: #fff; border: 1px solid var(--rule); }
th { text-align: left; font-weight: 600; color: var(--dim); font-size: 12px; padding: 8px 10px; border-bottom: 1px solid var(--rule); }
td { padding: 8px 10px; border-top: 1px solid var(--rule); vertical-align: top; }
td span { color: var(--dim); font-size: 12px; }
td.n { text-align: right; font-variant-numeric: tabular-nums; }
tr.good td:nth-child(3) { color: var(--good); } tr.bad td:nth-child(3) { color: var(--bad); font-weight: 600; }
td.pics { border-top: 0; padding-top: 0; } td.pics img { width: 48%%; margin-right: 1%%; border: 1px solid var(--rule); }
</style></head><body>
<h1>The top 20 on JetSki</h1>
<p>Kosmos %(version)s on %(machine)s, %(when)s. %(loaded)d of %(count)d loaded. Each site in a fresh JetSki;
times from the machine's own log. Beside it, this Mac's Chrome, headless, whole process.%(before)s</p>
<table><tr><th>#</th><th>Site</th><th>Ended</th><th>JetSki</th><th>Then</th><th>Chrome</th>
<th>Kept out</th><th>Own cookies</th><th>Others'</th></tr>
%(rows)s
</table></body></html>
""" % {"version": html.escape(run["version"]), "machine": html.escape(run["machine"]),
       "when": html.escape(run["when"]), "loaded": loaded, "count": len(run["sites"]),
       "before": (" <i>Then</i> is against %s." % html.escape(before["version"])) if before else "",
       "rows": "\n".join(rows)}
    path = os.path.join(out_dir, "report.html")
    with open(path, "w") as f:
        f.write(page)
    return path


def main(argv):
    chosen = arguments(argv)
    machine = chosen["machine"]
    machine_name = "m700" if machine.startswith("192.168.1.35") else machine.replace(":", "-")
    when = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
    run_name = "%s-%s-%s" % (version(), machine_name, datetime.datetime.now().strftime("%m%d-%H%M"))
    out_dir = os.path.join(ROOT, "build", "top20", run_name)
    os.makedirs(out_dir, exist_ok=True)

    session = kosmos_telnet.Session(machine)
    run = {"version": version(), "machine": machine_name, "when": when, "wait": chosen["wait"], "sites": []}
    try:
        kept = cookies_of(session, os.path.join(out_dir, "cookies-0.db"))
        for rank, name, site in SITES:
            if chosen["only"] and site.split(".")[0] not in chosen["only"] and site not in chosen["only"]:
                continue
            print("top20: %2d %-20s" % (rank, name), end="", flush=True)
            result = visit(session, site, chosen["wait"])
            result.update({"rank": rank, "name": name, "site": site})
            picture = "%02d-%s-jetski.png" % (rank, site.split(".")[0])
            if screen(machine, os.path.join(out_dir, picture)):
                result["picture"] = picture
            now = cookies_of(session, os.path.join(out_dir, "cookies-%02d.db" % rank))
            new = now - kept
            kept = now
            result["cookies_own"] = sum(1 for d, _, _ in new if own(d, site))
            result["cookies_others"] = len(new) - result["cookies_own"]
            result["cookie_sites"] = sorted({d.lstrip(".") for d, _, _ in new if not own(d, site)})
            if chosen["chrome"]:
                shot = "%02d-%s-chrome.png" % (rank, site.split(".")[0])
                result["chrome_seconds"] = chrome(site, os.path.join(out_dir, shot))
                if result["chrome_seconds"] is not None:
                    result["chrome_picture"] = shot
            run["sites"].append(result)
            result["within"] = (result["ended"] == "loaded" and result["seconds"] is not None
                                and result["seconds"] <= STANDARD)
            print(" %-26s %6s  kept out %4d  cookies %3d own, %3d others'%s"
                  % (result["ended"], "%.1f s" % result["seconds"] if result["seconds"] is not None else "-",
                     result["kept_out"], result["cookies_own"], result["cookies_others"],
                     "" if result["within"] else "  OVER %.0f s" % STANDARD), flush=True)
        session.run("kill jetski")
    finally:
        session.close()

    bench = os.path.join(ROOT, "bench", "top20")
    os.makedirs(bench, exist_ok=True)
    kept_file = os.path.join(bench, "%s-%s.json" % (run["version"], machine_name))
    before = previous(machine_name, kept_file)
    with open(kept_file, "w") as f:
        json.dump(run, f, indent=1)
    with open(os.path.join(out_dir, "results.json"), "w") as f:
        json.dump(run, f, indent=1)
    page = report(run, before, out_dir)
    loaded = sum(1 for s in run["sites"] if s["ended"] == "loaded")
    print("top20: %d of %d loaded; %s; kept as %s" % (loaded, len(run["sites"]), page,
                                                     os.path.relpath(kept_file, ROOT)))
    over = [s for s in run["sites"] if not s.get("within")]
    if over:
        print("top20: FAIL: %d of %d not loaded within %.0f s: %s"
              % (len(over), len(run["sites"]), STANDARD,
                 ", ".join("%s %s" % (s["name"], "%.1f s" % s["seconds"] if s["seconds"] is not None
                                      else s["ended"]) for s in over)))
        return 1
    print("top20: PASS: every site loaded within %.0f s" % STANDARD)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
