# Privacy and Ad Blocking: adverts and trackers kept out of JetSki

Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.

Designed on 16 November 2026, before it is built. The window is
`docs/adblock.html`, the diagram `docs/adblock-architecture.png` (drawn
from `docs/adblock-architecture.html`).

Diego, 16 November 2026: "We need a way to have adblocking plugin for
browsing to avoid loading and saving in unnecessary data", "I can't
believe the verge uses 100 cookies", "I want to be able to avoid tracking
cookies and else", "Can you see how Adblock works and replicate it and
create our own Adblock extension for jetski?" - pointing at eyeo's
extensions, the source of AdBlock and Adblock Plus.

**In short.** WebKit already has an ad blocker's engine in it: the one
Safari's content blockers run on. It is switched off in our port. Blocking
turns it on. A Kosmos library converts the public filter lists (EasyList,
EasyPrivacy, the cookie-banner list) into its rules. Third-party cookies
are refused, which the Settings window agreed on 14 November already says
and the code never did. JetSki gets a shield in its toolbar that says what
was kept out, and turns blocking off for one site.

**It was already in JetSki's feature set**, agreed on 11 November
(`webkit.md`: "Ads and trackers blocked, on by default and per site",
EasyList's lists into WebKit's content blocker rules), and it never
reached the roadmap or a step. This document is that line, made into a
design. And the engine is a step Mail needs anyway: Mail on WebKit's W4
holds a message's network pictures with a content rule
(`mailweb.md`). So B1 below is Mail's W4 and this design's first step at
once, built one time.

---

## Why: what one page costs now

The M700's `cookies.db` after loading The Verge (`testing.md` 18.837):

| | |
|---|---|
| cookies kept | 235 |
| sites they belong to | 98 |
| The Verge's own (theverge.com, voxmedia) | 29 |
| everybody else's | 206 |

The most from one site was pubmatic.com's 38. Then sonobi.com,
rubiconproject.com, casalemedia.com, linkedin.com and tapad.com. Every one
of those sites is on EasyList or EasyPrivacy. Each is a request JetSki
made, a script it ran, and with 0.11.441 a cookie it saved to the disk.

---

## How Adblock does it

AdBlock and Adblock Plus are one product, from eyeo. Their repository
(`gitlab.com/eyeo/browser-extensions-and-premium/extensions/extensions`,
GPLv3) is a monorepo: both extensions are built as hosts around eyeo's
engine, the *web extension ad filtering solution*. That engine is where
the work is, and it does four things:

1. **Filter lists, subscribed to.** EasyList (adverts), EasyPrivacy
   (trackers), and others by language and purpose. Each is a text file
   fetched over HTTPS and refetched when its `! Expires:` line says so,
   typically every four days. Adblock Plus also subscribes to *Acceptable
   Ads*, eyeo's paid allowlist, by default.
2. **Request filters** - `||doubleclick.net^`, `/ads/banner*`,
   `$third-party`, `$script`, `$domain=example.com`, and `@@` for
   exceptions. In Manifest V3 they are converted into Chrome's
   `declarativeNetRequest` rules, which the browser matches before a
   request leaves.
3. **Element hiding** - `example.com##.ad-slot`, and `##.ad-slot` for
   every site. A content script injects the selectors as a style sheet at
   `document_start`.
4. **What a selector cannot say** - *element hiding emulation*
   (`#?#div:-abp-has(> .sponsored)`, `:-abp-contains(...)`), watched by a
   script as the page changes, and *snippets*: small scripts a filter
   names, run in the page to defeat a particular site's tricks.

A browser extension, in other words, is a way to put a filter engine where
the browser asks "may this load?" and "what does this page look like?".
Kosmos has no extension runtime, and does not need one for this: **WebKit
asks those questions itself**.

## How Safari does it, which is how we do it

WebKit's *content extensions* (`Source/WebCore/contentextensions/`) are
the engine under every Safari content blocker. A rule list is JSON:

```json
[{"trigger": {"url-filter": "^https?://([^/]*\\.)?doubleclick\\.net[/:]",
              "load-type": ["third-party"]},
  "action": {"type": "block"}},
 {"trigger": {"url-filter": ".*", "if-domain": ["*theverge.com"]},
  "action": {"type": "css-display-none", "selector": ".ad-slot"}},
 {"trigger": {"url-filter": "^https?://([^/]*\\.)?pubmatic\\.com[/:]"},
  "action": {"type": "block-cookies"}}]
```

WebKit compiles a list once into a DFA's bytecode, kept in a file, and
every web process matches each request's address against it before the
request is made: a pass over the address's bytes, microseconds, with no
script involved. The actions are:
- `block` - the request is never made;
- `block-cookies` - the request goes, without cookies, and any it is
  answered with are not kept;
- `css-display-none` - a selector hidden, per site or everywhere;
- `ignore-previous-rules` - an exception, which is how one site is let
  through;
- `make-https` - an address upgraded.

**This is how eyeo did it too**, for Adblock Plus for Safari: their
`abp2blocklist` converted EasyList into exactly this JSON. So the design
is not new; it is the one that already shipped on WebKit, by the people
Diego pointed at.

What the conversion cannot carry is part 4 above: element hiding emulation
and snippets. They are a few percent of EasyList's rules, and they are
step B7, done with WebKit's user scripts if they turn out to be missed.

---

## Feature set

What a person can do:

1. **Browse with adverts and trackers kept out**, on by default, in every
   tab. Blocked requests are never made, so their bytes never arrive,
   their scripts never run and their cookies never reach the disk.
2. **Refuse third-party cookies**, on by default: a cookie set by a site
   other than the one in the address field is not kept. This is Safari's
   default, and JetSki's Settings, agreed on 14 November, already says it.
3. **See what was kept out**, from a shield in the toolbar: how many
   requests on this page, how many of them trackers, how many cookies
   refused, and an estimate of the bytes not downloaded.
4. **Turn blocking off for one site**, from the shield, for a site that
   breaks or that you want to support. It is on again from the same place
   or from Settings. The page reloads at once, and nothing is compiled
   again: WebKit's own per-navigation switch (B4).
5. **Choose lists** in Settings, *Blocking*: EasyList (adverts),
   EasyPrivacy (trackers), EasyList Cookie (the "we use cookies" banners),
   each on or off, with when it was last fetched and how many rules it
   gave. Language lists can be added from the same place.
6. **Write your own filters**, in Adblock Plus syntax, in a text field:
   `||annoying.example^`, `example.com##.popup`.
7. **Lists stay current by themselves**: each is fetched again when its
   `Expires` says, in the background, and *Update now* does it at once.
8. **Clear what trackers already left**: the first time blocking runs,
   every cookie whose site a list blocks is removed, by itself. On the
   M700 today that is 206 of 235. A button does it again later.
9. **Mail gets the same lists** (`docs/mailweb.md`): a message's tracking
   pixels are kept out like a page's, from the same compiled file.
10. **From the prompt**: `filters` lists the lists and their rules,
    `filters update` fetches them, and `filters test URL [PAGE]` says
    whether that address would be blocked on that page, and by which
    rule.
11. **Cookies allowed one site at a time**: a site that needs third
    parties' cookies - a sign-in spread across domains - is given them,
    from the shield or from Settings, and every other site still refuses
    them.
12. **A preferences panel, and cookies managed** (Diego, 16 November:
    "Make sure add a preferences panel for privacy and ad blocking in jet
    ski so the user can change settings, manage cookies, etc"). JetSki's
    Settings has a group of its own, *Privacy and Ad Blocking*: blocking on
    and off, third-party cookies, the lists, Acceptable Ads, your own
    filters, allowed sites, cookies allowed, and *When JetSki closes*
    (keep cookies, or clear all but the sites kept). *Manage cookies*
    lists every site JetSki keeps cookies for - how many, how large, when
    last used, a site you visited or a tracker that came along - opens a
    site to its cookies by name, domain and expiry (never their values),
    removes one cookie, a site's, the trackers', or all, and stars a site
    to keep. Every change applies to open tabs at once. Drawn in
    `docs/adblock.html`, *Settings* and *Manage cookies*.

Not in it, deliberately:
- **Acceptable Ads on by default.** It is eyeo's business, a paid
  allowlist. It is offered as a list a person can turn on, and is off
  (Diego, 16 November: "default is we dont show any banners").
- **An extension runtime.** Kosmos's applications are kits put together
  (*What this is aiming at*); blocking is a kit WebKit already has, not a
  WebExtensions API to implement.

---

## Architecture

### The pieces, and what supplies each

| piece | supplied by | new? |
|---|---|---|
| matching a request against rules | WebKit's content extensions, in each web process | on in our port: `ENABLE_CONTENT_EXTENSIONS`, one line in `OptionsKosmos.cmake` |
| compiling rules into bytecode | WebKit's `ContentExtensionCompiler`, in JetSki's process (the UI process) on a WebKit work queue | no |
| the compiled file, shared with every web process | WebKit's `ContentRuleListStore`: a file, mapped, its pages handed over as shared memory | no; needs our `mmap` of a file, to be checked (step B1) |
| fetching the lists | the Network Kit, over HTTPS through the system's TLS | no |
| **converting Adblock Plus syntax into WebKit's JSON** | **`/Kosmos/Libraries/filters.lua`** | **new** |
| refusing third-party cookies | curl's `CookieJarDB`, `CookieAcceptPolicy::OnlyFromMainDocumentDomain`, set through `WKHTTPCookieStoreSetHTTPCookieAcceptPolicy` | no; the WebKit Kit sets it |
| counting what was blocked | `WKPageNavigationClient`'s `contentRuleListNotification`, one call per rule that acted | no; the WebKit Kit passes it on as an event |
| the shield, its pop-over, Settings' *Blocking* | JetSki, with the UI kit | new, in JetSki |
| `filters` at the prompt | a program over `filters.lua` | new |

**The one new kit is the converter**, and it is a kit and not part of
JetSki because Mail wants the same lists, and because `filters` at the
prompt is a third user. Both ask *does another application want this?*
and the answer is yes before a line is written.

### Lua, not C, and why

The converter is string handling over a text file: read EasyList's lines,
recognise each filter's kind, and write a JSON rule. That is exactly the
kind of code the language split keeps in Lua: paths and patterns, where a
C bug is an overflow and a Lua bug is a stack trace. It runs once per
list update, every few days, never on a page's path. Lua's own `string`
functions are C, so a line costs a few pattern matches.

**It runs on the host too**, through `tools/host_lua.c`, as `kfs.lua`
does. So `make test` checks the conversion without booting anything, and
the build can carry a converted copy of each list. A machine then blocks
from its first boot, before it has fetched anything.

If B2 measures the conversion of all three lists at over a second on the
M700, the line-by-line loop moves to C. That is the measure that decides;
the guess is about 0.3 s for EasyList's 80,000 lines.

### Where things are kept

```
/Kosmos/Filters/            built into the image: each list, converted
  easylist.json               at build time - blocking from the first boot
  easyprivacy.json
  easylist-cookie.json
/Home/Settings/Filters/     the person's, shared by JetSki and Mail
  lists                       which lists are on, and their addresses
  own.txt                     filters typed in Settings
  allowed                     sites blocking is off for
  easylist.txt                a list as fetched, with its Expires
  easylist.json               ... and as converted
  Rules/                      WebKit's compiled files, one per list
```

### Data: a region or a message

The compiled rules are WebKit's: a file of bytecode, mapped once per web
process and shared. A page's requests are matched against those bytes
where they are; nothing travels per request. What crosses to JetSki is
one notification per rule that acted - an address and a rule's
identifier, a one-shot message of the kind *Control by message* allows -
and JetSki adds it to the page's count. Nothing about blocking is a stream.

### Each list compiled alone, and sites allowed by WebKit's own switch

WebKit takes several compiled lists at once and applies each on its own. So:
- **one compiled file per list**, so turning EasyPrivacy off compiles
  nothing;
- **a site allowed is not a list.** This said it was - one rule per site,
  `ignore-previous-rules` with `if-top-url`, a list of its own, last - and
  that cannot work: `ignore-previous-rules` reaches only the rules before
  it *in the same list*, and every list is evaluated alone (corrected 16
  November, before B4 was built). A site allowed is WebKit's
  per-navigation switch, Safari's: the navigation's decision handed back
  with website policies whose content blockers are off
  (`WKWebsitePoliciesSetContentBlockersEnabled`). Nothing is compiled;
- **your own filters** are a list of their own.

### The busiest path: a page's request

1. WebCore is about to make a request for a page.
2. `ContentExtensionsBackend` runs the address through each compiled
   list's bytecode, in the web process, already in memory: a few
   microseconds.
3. **Blocked**: the request is never made, so there is no network process
   round trip, no DNS lookup, no TLS handshake, no bytes, no cookie and
   no script. The rule's identifier goes to JetSki.
4. **Not blocked**: it goes as before, cookies stripped if a
   `block-cookies` rule said so.

The cost of blocking is step 2, a few microseconds a request. A blocked
request saves a DNS lookup (30 to 180 ms each in the M700's log of 15 November), a TLS
handshake and its bytes. **Blocking makes a page faster; it costs nothing
measurable.**

The slow part is compiling, and it is off the page's path: once per list
update, on a WebKit work queue, while JetSki goes on drawing. For EasyList
on Safari's hardware it is a few seconds; on the M700 the three lists take
70 s the first time and 3.6 s to be found compiled after (B2). Until a
compile finishes the previous compiled file goes on being used.

---

## Steps

Each leaves a permanent test, and each is measured on The Verge on the
M700: requests made, bytes, cookies kept, load time.

- **B1. The engine on.** `ENABLE_CONTENT_EXTENSIONS` in our port. The
  WebKit Kit takes a list of rule files and applies them to every page it
  makes. Check that the compiled file maps into a web process on Kosmos.
  Test: `x86-jetski` serves a page whose `ad.js` and `track.gif` a
  hand-written list blocks; the server must never be asked for them, and
  the page must still paint. Control: the list off, and both are asked
  for. **Built in 0.11.442** (`testing.md` 18.838): the compiled list is
  copied into a region the web process maps read only, and WebKit patch
  0033 tells the application what a list kept out.
- **B2. The converter.** `filters.lua`: Adblock Plus syntax into WebKit's
  JSON, request filters, exceptions, options, element hiding. Host tests
  against filters taken from EasyList's own syntax and their expected
  rules. Measured: converting and compiling each list on the M700.
  **Built in 0.11.443** (`testing.md` 18.839): the three lists converted
  on the Mac at build time (about 0.6 s each, so the loop stays Lua) and
  carried gzipped in FULL images, compiled on a machine's first launch
  and found compiled after, by their bytes' hash. It found an IPC bug: a
  message with two regions handed each under the other's name. Under TCG
  the three compile in 89 s; on the M700, 16 November, in 70 s - the
  Cookie list ready at 14.5 s, EasyList at 44 s, EasyPrivacy at 70 s, one
  after another on WebKit's queue while the page went on - and JetSki
  opened again found all three compiled in 3.6 s.
- **B3. Third-party cookies refused**, and `block-cookies` from
  EasyPrivacy, with sites allowed them one at a time (a patch to
  `CookieJarDB`), and the trackers' cookies cleared the first time. Test: a page on one host that a second host sets a cookie
  for; the second's cookie is not in `cookies.db`. Control: the policy
  `Always`, and it is.
- **B4. The shield.** Counts from `contentRuleListNotification`, the
  pop-over, *Off for this site*. **Corrected 16 November, before it is
  built**: an allowlist compiled alone cannot do it - WebKit evaluates
  each rule list on its own, and `ignore-previous-rules` reaches only the
  rules before it in the same list. *Off for this site* is WebKit's own
  per-navigation switch instead, Safari's: `WKWebsitePoliciesSetContent
  BlockersEnabled(false)` handed with the navigation's decision
  (`WKFramePolicyListenerUseWithPolicies`) for a site on the list. Nothing
  is compiled, and the page reloads.
- **B5. The preferences panel** (feature 12): Settings' *Privacy and Ad
  Blocking* - blocking, third-party cookies, lists, own filters, allowed
  sites, cookies allowed, *When JetSki closes* - and *Manage cookies*,
  read and removed through WebKit's cookie store
  (`WKHTTPCookieStoreGetHTTPCookies`, `DeleteHTTPCookie`), so the network
  process that owns `cookies.db` is the only thing that writes it. The
  settings are a file in `/Home/Settings/Filters`, read by the WebKit Kit
  for every view, and the same file Mail reads. Test: a setting changed
  in the panel, read back by the kit; a site's cookies listed, one
  removed, and gone from `cookies.db`. Control: the removal skipped, and
  the cookie is still there.
- **B6. Lists fetched and kept current**, in the background, when each
  one's `Expires` says; `filters update` at the prompt.
- **B7. Element hiding emulation and snippets**, through WebKit's user
  scripts, if pages show they are missed.
- **B8. Mail**, from the same compiled files.

---

## Decided by Diego, 16 November 2026

1. **The name: Privacy and Ad Blocking** ("Privacy and Ad Blocking").
   Settings' group is called that, and the shield's pop-over is headed
   with it.
2. **The default lists: EasyList, EasyPrivacy and EasyList Cookie, all
   on** ("yes").
3. **Acceptable Ads: offered, off by default** ("offer it to switch on and
   off but default is we dont show any banners"). A row in Settings' lists,
   eyeo's list, off until a person turns it on.
4. **The trackers' cookies already kept are cleared by themselves**, the
   first time blocking runs, **and cookies can be allowed one site at a
   time** ("clear them and have a way to opt out for cookies on a
   site-basis"). Each site has its own cookie switch: in the shield's
   pop-over, *Allow cross-site cookies on this site*, and in Settings
   under *Cookies allowed*. A site on that list may have third parties'
   cookies kept while it is the page, which some sign-ins need. This is
   a Kosmos patch to curl's `CookieJarDB`: its accept policy is one value
   for every page, and asking it per site is `checkCookieAcceptPolicy`
   looking the page's site up in that list (step B3). The *Clear* button
   stays for later.
