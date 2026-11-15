/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **JetSki's UI process** (`jetski.h`, `docs/w7.md` W7c): WebKit2's view,
 * as `webkit2test.cpp` drives it, made into something a window can hold -
 * one page, loaded by address, whose run loop is run a slice at a time
 * between the window's own events, and whose picture is painted into the
 * window's surface when it changes.
 *
 * The web and network processes are this image again, in their roles
 * (`kosmos_project_role`), as the suite's are.
 *
 * **A slice ends early when there is a picture**: WebKit says the view
 * needs painting, the loop is stopped, and the window paints it at once -
 * so a page appears as it arrives rather than at the slice's end.
 */

#include "jetski.h"

#include <kosmos.h>                 // kosmos_wait, struct process_death
#include <stdio.h>
#include <string.h>

#include <WebKit/WKContext.h>
#include <WebKit/WKContextConfigurationRef.h>
#include <WebKit/WKErrorRef.h>
#include <WebKit/WKFrameInfoRef.h>
#include <WebKit/WKFramePolicyListener.h>
#include <WebKit/WKNavigationActionRef.h>
#include <WebKit/WKPage.h>
#include <WebKit/WKPageConfigurationRef.h>
#include <WebKit/WKPageKosmos.h>
#include <WebKit/WKPageNavigationClient.h>
#include <WebKit/WKPagePrivate.h>
#include <WebKit/WKPageUIClient.h>
#include <WebKit/WKRetainPtr.h>
#include <WebKit/WKString.h>
#include <WebKit/WKURL.h>
#include <WebKit/WKURLRequest.h>
#include <WebKit/WKView.h>
#include <WebKit/WKViewClient.h>
#include <WebKit/WKWebsiteDataStoreConfigurationRef.h>
#include <WebKit/WKWebsiteDataStoreRef.h>
#include <stdlib.h>
#include <string.h>
#include <wtf/MonotonicTime.h>
#include <wtf/RunLoop.h>
#include <wtf/Seconds.h>
#include <wtf/Vector.h>
#include <memory>

extern "C" int kosmos_webkit_role(unsigned long role);

/* A web or network process arrives here (`user/init/main.c`). */
extern "C" int kosmos_project_role(unsigned long role)
{
    return kosmos_webkit_role(role);
}

namespace {

/*
 * **A tab** (`docs/webkit.html`, agreed 11 November 2026): a view and its
 * page, a web process of its own, and what its page has said since the
 * last pump. The window shows one, `browser.tab`; the rest go on in the
 * dark, unseen and inactive, as a browser's background tabs do.
 */
struct Tab {
    int id = 0;
    WKViewRef view = nullptr;
    WKPageRef page = nullptr;
    unsigned happened = 0;
    int error = 0;
    bool loading = false;
    bool asked = false;             // a link followed, its load not begun
    WTF::MonotonicTime askedAt;
    char going[2048] { };
    long pid = 0;                   // its web process, the last one seen
    bool crashed = false;           // that process ended, since the last load
    long crashedPid = 0;
    char crashedAddress[2048] { };
};

/*
 * **The children, collected** (Diego, 14 November 2026: "yes fix the
 * zombie processes too"). The web and network processes are JetSki's
 * children - `ProcessLauncherKosmos.cpp` spawns them - and one that ended
 * stayed in Nebula's table until JetSki did, because nothing waited for it.
 * Every pump collects whoever has ended, without waiting, and keeps the
 * last few deaths by id: a tab whose page stopped asks for its own.
 */
struct Death {
    long pid = 0;
    long code = 0;
    struct process_death death { };
};

constexpr unsigned deathsKept = 16;
Death deaths[deathsKept];
unsigned deathsNext = 0;

void collect()
{
    for (;;) {
        struct wait_result got;
        memset(&got, 0, sizeof got);
        if (kosmos_wait(&got, 1) != 0)
            return;
        auto& d = deaths[deathsNext++ % deathsKept];
        d.pid = static_cast<long>(got.id);
        d.code = static_cast<long>(got.code);
        d.death = got.death;
        d.death.name[sizeof d.death.name - 1] = '\0';
        if (got.death.faulted)
            fprintf(stderr, "jetski: process %ld ended: %s at %#lx, touching %#lx, thread %u\n",
                d.pid, d.death.name, static_cast<unsigned long>(d.death.pc),
                static_cast<unsigned long>(d.death.addr), d.death.thread);
        else
            fprintf(stderr, "jetski: process %ld ended, code %ld\n", d.pid, d.code);
    }
}

const Death* deathOf(long pid)
{
    for (auto& d : deaths) {
        if (pid && d.pid == pid)
            return &d;
    }
    return nullptr;
}

struct Browser {
    Vector<std::unique_ptr<Tab>> tabs;
    Tab* tab = nullptr;             // the one in the window
    int nextId = 1;
    int opened = 0;                 // a tab a page opened, not yet told
    int closing = 0;                // a tab whose page asked to be closed
    unsigned width = 0, height = 0;
    bool running = false;           // inside `jetski_pump`'s run loop
    WTF::RunLoop::Timer* slice = nullptr;

    void stop()
    {
        if (running) {
            running = false;
            WTF::RunLoop::mainSingleton().stop();
        }
    }

    Tab* find(int id)
    {
        for (auto& tab : tabs) {
            if (tab->id == id)
                return tab.get();
        }
        return nullptr;
    }
};

Browser browser;

Tab* tabOf(const void* clientInfo)
{
    return const_cast<Tab*>(static_cast<const Tab*>(clientInfo));
}

// What the window shows is told at once; a tab in the dark waits its turn.
void told(Tab* tab, unsigned what)
{
    tab->happened |= what;
    if (tab == browser.tab || what & (JETSKI_FINISHED | JETSKI_FAILED | JETSKI_CRASHED))
        browser.stop();
}

void copy(WKStringRef, char* into, size_t room);

void didStart(WKPageRef, WKNavigationRef, WKTypeRef, const void* info)
{
    auto* tab = tabOf(info);
    tab->crashed = false;
    tab->loading = true;
    tab->asked = false;
    told(tab, JETSKI_STARTED);
}

/*
 * **A link, the moment it is followed** (Diego, 14 November 2026: "right
 * now i click some links and nothing happens"). WebKit says a navigation
 * began only once it is decided and, for another site, a web process has
 * been started for it - on the M700 a second or more after the click, with
 * nothing on screen. Asking is the first thing that happens, so the page is
 * loading from here: for the page itself, not a frame inside it.
 */
void decidePolicy(WKPageRef, WKNavigationActionRef action, WKFramePolicyListenerRef listener, WKTypeRef, const void* info)
{
    if (WKNavigationActionShouldPerformDownload(action)) {
        WKFramePolicyListenerDownload(listener);
        return;
    }

    auto frame = adoptWK(WKNavigationActionCopyTargetFrameInfo(action));
    if (frame && WKFrameInfoGetIsMainFrame(frame.get())) {
        auto* tab = tabOf(info);
        tab->asked = true;
        tab->askedAt = WTF::MonotonicTime::now();
        tab->going[0] = '\0';
        auto request = adoptWK(WKNavigationActionCopyRequest(action));
        auto url = request ? adoptWK(WKURLRequestCopyURL(request.get())) : nullptr;
        if (url) {
            auto string = adoptWK(WKURLCopyString(url.get()));
            copy(string.get(), tab->going, sizeof tab->going);
        }
        told(tab, JETSKI_ASKED);
    }
    WKFramePolicyListenerUse(listener);
}

// A link to a place on the same page, `#notes`: asked, and never loaded.
void didSameDocument(WKPageRef, WKNavigationRef, WKSameDocumentNavigationType, WKTypeRef, const void* info)
{
    tabOf(info)->asked = false;
}

void didCommit(WKPageRef, WKNavigationRef, WKTypeRef, const void* info)
{
    told(tabOf(info), JETSKI_COMMITTED);
}

void didFinish(WKPageRef, WKNavigationRef, WKTypeRef, const void* info)
{
    auto* tab = tabOf(info);
    tab->loading = false;
    tab->asked = false;
    told(tab, JETSKI_FINISHED);
}

void didFail(WKPageRef, WKNavigationRef, WKErrorRef error, WKTypeRef, const void* info)
{
    auto* tab = tabOf(info);
    tab->error = error ? WKErrorGetErrorCode(error) : 0;
    tab->loading = false;
    tab->asked = false;
    told(tab, JETSKI_FAILED);
}

void didCrash(WKPageRef page, const void* info)
{
    auto* tab = tabOf(info);
    tab->loading = false;
    tab->crashed = true;
    tab->crashedPid = tab->pid;
    tab->crashedAddress[0] = '\0';
    if (tab->asked)
        memcpy(tab->crashedAddress, tab->going, sizeof tab->crashedAddress);
    else if (auto url = adoptWK(WKPageCopyActiveURL(page))) {
        auto string = adoptWK(WKURLCopyString(url.get()));
        copy(string.get(), tab->crashedAddress, sizeof tab->crashedAddress);
    }
    fprintf(stderr, "jetski: tab %d's page process %ld ended, at %s\n",
        tab->id, tab->crashedPid, tab->crashedAddress);
    told(tab, JETSKI_CRASHED);
}

void needsDisplay(WKViewRef, WKRect, const void* info)
{
    told(tabOf(info), JETSKI_PICTURE);
}

/*
 * **The window a page is in** (`jetski_frame`): what `window.outerWidth`,
 * `screenX` and the rest answer. Without it they are all nought, which no
 * browser a person uses says and which a site's robot check reads as a
 * browser with no window at all - the mark of one driven by a program.
 */
WKRect windowFrame;

WKRect getWindowFrame(WKPageRef, const void*)
{
    return windowFrame;
}

WKContextRef context()
{
    static WKContextRef shared = [] {
        auto configuration = adoptWK(WKContextConfigurationCreate());
        return WKContextCreateWithConfiguration(configuration.get());
    }();
    return shared;
}

/*
 * **Where cookies are kept**: a file of curl's `CookieJarDB`, on SQLite, in
 * `/Home/Settings/JetSki` beside the cache - which only the network
 * process reaches (`WebsiteDataStoreKosmos.cpp`). WebKit names a cookie
 * file for a data store only on Cocoa; everywhere else the network process
 * falls back to `cookie.jar.db` in no folder at all, which on Kosmos is
 * nowhere, so no cookie was ever kept - not even for the page that set it,
 * and Google's script, setting one and reloading, came back a stranger.
 */
WKWebsiteDataStoreRef dataStore()
{
    static WKWebsiteDataStoreRef shared = [] {
        auto configuration = adoptWK(WKWebsiteDataStoreConfigurationCreate());
        auto path = adoptWK(WKStringCreateWithUTF8CString("/Home/Settings/JetSki/cookies.db"));
        WKWebsiteDataStoreConfigurationSetCookieStorageFile(configuration.get(), path.get());
        return WKWebsiteDataStoreCreateWithConfiguration(configuration.get());
    }();
    return shared;
}

void copy(WKStringRef string, char* into, size_t room)
{
    into[0] = '\0';
    if (string) {
        WKStringGetUTF8CStringNonStrict(string, into, room);
        into[room - 1] = '\0';
    }
}

// The window's tab shown, and every other one dark.
void show(Tab* tab)
{
    for (auto& each : browser.tabs) {
        bool shown = each.get() == tab;
        WKViewSetVisible(each->view, shown);
        WKViewSetActive(each->view, shown);
        if (!shown)
            WKViewSetFocus(each->view, false);
    }
    browser.tab = tab;
    if (tab)
        tab->happened |= JETSKI_PICTURE;
}

WKPageRef createNewPage(WKPageRef, WKPageConfigurationRef, WKNavigationActionRef, WKWindowFeaturesRef, const void*);
void closePage(WKPageRef, const void*);

/*
 * **A tab, from a configuration**: JetSki's own for one a person opens,
 * and the one WebKit hands over for one a page opens - `target=_blank`,
 * `window.open` - which carries the page that opened it.
 */
Tab* makeTab(WKPageConfigurationRef configuration)
{
    auto tab = std::make_unique<Tab>();
    tab->id = browser.nextId++;
    tab->view = WKViewCreate(configuration);
    if (!tab->view)
        return nullptr;
    WKViewSetSize(tab->view, WKSizeMake(browser.width, browser.height));

    WKViewClientV0 viewClient { };
    viewClient.base = { 0, tab.get() };
    viewClient.setViewNeedsDisplay = needsDisplay;
    WKViewSetViewClient(tab->view, &viewClient.base);

    tab->page = WKViewGetPage(tab->view);
    WKPageNavigationClientV0 navigation { };
    navigation.base = { 0, tab.get() };
    navigation.didStartProvisionalNavigation = didStart;
    navigation.didCommitNavigation = didCommit;
    navigation.didFinishNavigation = didFinish;
    navigation.didFailNavigation = didFail;
    navigation.didFailProvisionalNavigation = didFail;
    navigation.webProcessDidCrash = didCrash;
    navigation.decidePolicyForNavigationAction = decidePolicy;
    navigation.didSameDocumentNavigation = didSameDocument;
    WKPageSetPageNavigationClient(tab->page, &navigation.base);

    WKPageUIClientV8 ui { };
    ui.base = { 8, tab.get() };
    ui.getWindowFrame = getWindowFrame;
    ui.createNewPage = createNewPage;
    ui.close = closePage;
    WKPageSetPageUIClient(tab->page, &ui.base);

    auto* made = tab.get();
    browser.tabs.append(WTF::move(tab));
    return made;
}

Tab* makeTab()
{
    auto configuration = adoptWK(WKPageConfigurationCreate());
    WKPageConfigurationSetContext(configuration.get(), context());
    WKPageConfigurationSetWebsiteDataStore(configuration.get(), dataStore());
    return makeTab(configuration.get());
}

// A link to a new window, or `window.open`: a new tab, beside, and shown.
WKPageRef createNewPage(WKPageRef, WKPageConfigurationRef configuration, WKNavigationActionRef, WKWindowFeaturesRef, const void*)
{
    auto* tab = makeTab(configuration);
    if (!tab)
        return nullptr;
    browser.opened = tab->id;
    show(tab);
    browser.stop();
    return static_cast<WKPageRef>(const_cast<void*>(WKRetain(tab->page)));
}

// `window.close()`: the window says which tab, and closes it.
void closePage(WKPageRef, const void* info)
{
    browser.closing = tabOf(info)->id;
    browser.stop();
}

WKPageRef current()
{
    return browser.tab ? browser.tab->page : nullptr;
}

} // namespace

extern "C" int jetski_open(unsigned width, unsigned height)
{
    if (!browser.tabs.isEmpty() || !width || !height)
        return browser.tabs.isEmpty() ? 1 : 0;

    browser.width = width;
    browser.height = height;

    // The first view first: making it is what starts WTF's main thread and
    // run loop, which the timer below is made on.
    auto* tab = makeTab();
    if (!tab)
        return 1;
    static WTF::RunLoop::Timer slice(WTF::RunLoop::mainSingleton(), "jetski slice"_s, [] {
        browser.stop();
    });
    browser.slice = &slice;
    show(tab);
    WKViewSetFocus(tab->view, true);
    return 0;
}

extern "C" int jetski_tab_new(void)
{
    if (browser.tabs.isEmpty())
        return 0;
    auto* tab = makeTab();
    return tab ? tab->id : 0;
}

extern "C" int jetski_tab_select(int id)
{
    auto* tab = browser.find(id);
    if (!tab)
        return 0;
    show(tab);
    return 1;
}

/*
 * **A tab closed**: its page closed - which ends its web process, unless
 * another page shares it - and its view let go. The window chooses which
 * is shown next; the last one is never closed here.
 */
extern "C" int jetski_tab_close(int id)
{
    if (browser.tabs.size() < 2)
        return 0;
    for (size_t i = 0; i < browser.tabs.size(); i++) {
        auto& tab = browser.tabs[i];
        if (tab->id != id)
            continue;
        WKPageClose(tab->page);
        WKRelease(tab->view);
        if (browser.tab == tab.get())
            browser.tab = nullptr;
        browser.tabs.removeAt(i);
        if (!browser.tab)
            show(browser.tabs[std::min(i, browser.tabs.size() - 1)].get());
        return 1;
    }
    return 0;
}

extern "C" unsigned jetski_tabs(struct jetski_tab* out, unsigned room)
{
    unsigned n = 0;
    for (auto& tab : browser.tabs) {
        if (n == room)
            break;
        auto& each = out[n++];
        each.id = tab->id;
        each.shown = tab.get() == browser.tab;
        each.loading = tab->loading || tab->asked;
        each.pid = static_cast<int>(tab->pid);
        auto title = adoptWK(WKPageCopyTitle(tab->page));
        copy(title.get(), each.title, sizeof each.title);
        if (!each.title[0]) {
            auto url = adoptWK(WKPageCopyActiveURL(tab->page));
            auto string = url ? adoptWK(WKURLCopyString(url.get())) : nullptr;
            copy(string.get(), each.title, sizeof each.title);
        }
        // A page that stopped has no address left in WebKit: the one kept.
        if (!each.title[0] && tab->crashed)
            snprintf(each.title, sizeof each.title, "%s", tab->crashedAddress);
    }
    return n;
}

extern "C" void jetski_frame(int x, int y, unsigned width, unsigned height)
{
    windowFrame = WKRectMake(x, y, width, height);
}

extern "C" void jetski_resize(unsigned width, unsigned height)
{
    if (!width || !height)
        return;
    browser.width = width;
    browser.height = height;
    for (auto& tab : browser.tabs)
        WKViewSetSize(tab->view, WKSizeMake(width, height));
}

extern "C" void jetski_load(const char* address)
{
    if (!current() || !address)
        return;
    auto url = adoptWK(WKURLCreateWithUTF8CString(address));
    WKPageLoadURL(current(), url.get());
}

extern "C" void jetski_back(void)
{
    if (current() && WKPageCanGoBack(current()))
        WKPageGoBack(current());
}

extern "C" void jetski_forward(void)
{
    if (current() && WKPageCanGoForward(current()))
        WKPageGoForward(current());
}

extern "C" void jetski_reload(void)
{
    if (current())
        WKPageReload(current());
}

extern "C" void jetski_stop(void)
{
    if (browser.tab) {
        WKPageStopLoading(browser.tab->page);
        browser.tab->asked = false; // stopped before it began says nothing
    }
}

extern "C" void jetski_pump(unsigned ms, struct jetski_state* out)
{
    memset(out, 0, sizeof *out);
    collect();
    for (auto& each : browser.tabs) {
        if (auto pid = WKPageGetProcessIdentifier(each->page))
            each->pid = pid;
    }
    auto* tab = browser.tab;
    if (!tab)
        return;

    // Something already waiting to be said is said now, without a slice.
    if (!tab->happened && !browser.opened && !browser.closing && ms) {
        browser.running = true;
        browser.slice->startOneShot(WTF::Seconds::fromMilliseconds(ms));
        WTF::RunLoop::run();
        browser.running = false;
        browser.slice->stop();
        tab = browser.tab;
    }

    out->happened = tab->happened;
    out->error = tab->error;
    tab->happened = 0;
    tab->error = 0;

    // A tab in the dark that began, finished or ended: its tab is drawn
    // again, for its title and whether it is loading.
    for (auto& each : browser.tabs) {
        if (each.get() != tab && each->happened & ~JETSKI_PICTURE)
            out->happened |= JETSKI_TABS;
        if (each.get() != tab)
            each->happened = 0;
    }
    out->opened = browser.opened;
    out->closing = browser.closing;
    browser.opened = browser.closing = 0;

    // Asked and never begun - a navigation a script cancelled, say - is
    // not loading for ever: thirty seconds is longer than any start.
    if (tab->asked && WTF::MonotonicTime::now() - tab->askedAt > 30_s)
        tab->asked = false;
    out->loading = tab->loading || tab->asked;
    if (tab->asked)
        memcpy(out->going, tab->going, sizeof out->going);
    out->tab = tab->id;
    out->progress = WKPageGetEstimatedProgress(tab->page);
    out->can_back = WKPageCanGoBack(tab->page);
    out->can_forward = WKPageCanGoForward(tab->page);

    auto title = adoptWK(WKPageCopyTitle(tab->page));
    copy(title.get(), out->title, sizeof out->title);

    auto url = adoptWK(WKPageCopyActiveURL(tab->page));
    if (url) {
        auto string = adoptWK(WKURLCopyString(url.get()));
        copy(string.get(), out->address, sizeof out->address);
    }
}

extern "C" int jetski_crash(int id, struct jetski_crash* out)
{
    memset(out, 0, sizeof *out);
    auto* tab = browser.find(id);
    if (!tab || !tab->crashed)
        return 0;
    collect();
    out->pid = static_cast<int>(tab->crashedPid);
    memcpy(out->address, tab->crashedAddress, sizeof out->address);
    if (auto* d = deathOf(tab->crashedPid)) {
        out->known = 1;
        out->code = d->code;
        out->faulted = d->death.faulted != 0;
        memcpy(out->fault, d->death.name, sizeof out->fault);
        out->kind = d->death.kind;
        out->thread = d->death.thread;
        out->pc = d->death.pc;
        out->addr = d->death.addr;
        out->sp = d->death.sp;
        out->detail = d->death.detail;
    }
    return 1;
}

extern "C" void jetski_paint(uint32_t* pixels, unsigned width, unsigned height, unsigned pitch)
{
    if (!current())
        return;
    WKPagePaintKosmos(current(), pixels, width, height, pitch, WKRectMake(0, 0, width, height));
}

//
// **The pointer**: which buttons are held, as the DOM counts them, and the
// last press, so a second one soon after and close by is a double click -
// WebKit is told how many, it does not count them itself.
//
namespace {

struct Pointer {
    unsigned held = 0;
    int button = -2, x = 0, y = 0, clicks = 0;
    MonotonicTime at;
} pointer;

constexpr Seconds doubleClickTime = 500_ms;
constexpr int doubleClickRoom = 4;

unsigned modifiersOf(unsigned mods)
{
    return (mods & 1 ? kWKKosmosModifierShift : 0) | (mods & 2 ? kWKKosmosModifierAlt : 0)
        | (mods & 4 ? kWKKosmosModifierControl : 0) | (mods & 8 ? kWKKosmosModifierMeta : 0);
}

unsigned heldBit(int button)
{
    return button == JETSKI_LEFT ? 1 : button == JETSKI_RIGHT ? 2 : button == JETSKI_MIDDLE ? 4 : 0;
}

}

extern "C" void jetski_mouse(enum jetski_mouse action, enum jetski_button button, int x, int y, unsigned mods)
{
    if (!current())
        return;

    if (action == JETSKI_PRESS) {
        auto now = MonotonicTime::now();
        bool again = button == pointer.button && now - pointer.at < doubleClickTime
            && abs(x - pointer.x) <= doubleClickRoom && abs(y - pointer.y) <= doubleClickRoom;
        pointer.clicks = again ? pointer.clicks + 1 : 1;
        pointer.button = button;
        pointer.x = x;
        pointer.y = y;
        pointer.at = now;
        pointer.held |= heldBit(button);
        WKPageMouseKosmos(current(), kWKKosmosMouseDown, button, pointer.held, x, y, pointer.clicks, modifiersOf(mods));
    } else if (action == JETSKI_RELEASE) {
        pointer.held &= ~heldBit(button);
        WKPageMouseKosmos(current(), kWKKosmosMouseUp, button, pointer.held, x, y, pointer.clicks, modifiersOf(mods));
    } else {
        // A move with a button held is a drag of that button, as WebKit has it.
        int moving = pointer.held & 1 ? JETSKI_LEFT : pointer.held & 4 ? JETSKI_MIDDLE : pointer.held & 2 ? JETSKI_RIGHT : JETSKI_NONE;
        WKPageMouseKosmos(current(), kWKKosmosMouseMove, moving, pointer.held, x, y, 0, modifiersOf(mods));
    }
}

//
// **The wheel**: a notch is three of WebKit's lines, 40 pixels each, as
// the kit scrolls three rows a notch (`ui.WHEEL_ROWS`). Shift turns it
// sideways, as it does on every desktop.
//
extern "C" void jetski_wheel(int x, int y, int notches, unsigned mods)
{
    if (!current() || !notches)
        return;
    float ticks = notches, pixels = notches * 3 * 40;
    if (mods & 1)
        WKPageWheelKosmos(current(), x, y, pixels, 0, ticks, 0, modifiersOf(mods & ~1u));
    else
        WKPageWheelKosmos(current(), x, y, 0, pixels, 0, ticks, modifiersOf(mods));
}

//
// **Keys**: `keys.lua`'s numbers as the DOM's names and Windows's codes,
// which are what WebCore's editing commands are keyed by off the Mac -
// Backspace in a text field is virtual key 8, not the character.
//
namespace {

struct Named {
    int code;
    const char* key;
    const char* dom;
    int virtualKey;
};

// The keys that are not characters, by `keys.lua`'s numbers.
const Named namedKeys[] = {
    { -1, "ArrowUp", "ArrowUp", 0x26 }, { -2, "ArrowDown", "ArrowDown", 0x28 },
    { -3, "ArrowRight", "ArrowRight", 0x27 }, { -4, "ArrowLeft", "ArrowLeft", 0x25 },
    { -5, "Home", "Home", 0x24 }, { -6, "End", "End", 0x23 },
    { -7, "PageUp", "PageUp", 0x21 }, { -8, "PageDown", "PageDown", 0x22 },
    { -9, "Insert", "Insert", 0x2d }, { -10, "Delete", "Delete", 0x2e },
};

// Punctuation, unshifted and shifted, with the key it is on.
struct Punctuation {
    char plain, shifted;
    const char* dom;
    int virtualKey;
};

const Punctuation punctuation[] = {
    { ';', ':', "Semicolon", 0xba }, { '=', '+', "Equal", 0xbb }, { ',', '<', "Comma", 0xbc },
    { '-', '_', "Minus", 0xbd }, { '.', '>', "Period", 0xbe }, { '/', '?', "Slash", 0xbf },
    { '`', '~', "Backquote", 0xc0 }, { '[', '{', "BracketLeft", 0xdb }, { '\\', '|', "Backslash", 0xdc },
    { ']', '}', "BracketRight", 0xdd }, { '\'', '"', "Quote", 0xde },
};

const char shiftedDigits[] = ")!@#$%^&*(";

void send(const char* key, const char* dom, const char* text, int virtualKey, unsigned mods)
{
    WKPageKeyKosmos(current(), true, key, dom, text, virtualKey, modifiersOf(mods));
    WKPageKeyKosmos(current(), false, key, dom, nullptr, virtualKey, modifiersOf(mods));
}

}

extern "C" int jetski_key(int c)
{
    if (!current())
        return 0;

    // `keys.parts`: the key, and its modifiers in steps of 1024 below it.
    unsigned mods = (255 - c) / 1024;
    int key = c + 1024 * (int)mods;

    for (auto& named : namedKeys) {
        if (named.code == key) {
            send(named.key, named.dom, nullptr, named.virtualKey, mods);
            return 1;
        }
    }
    if (key <= -11 && key >= -22) {
        char name[4] = { 'F', 0, 0, 0 };
        int n = -10 - key;
        if (n >= 10) {
            name[1] = '1';
            name[2] = (char)('0' + n - 10);
        } else
            name[1] = (char)('0' + n);
        send(name, name, nullptr, 0x6f + n, mods);
        return 1;
    }
    if (key < 0 || key > 255)
        return 0;

    switch (key) {
    case 8: case 127:
        send("Backspace", "Backspace", nullptr, 0x08, mods);
        return 1;
    case 9:
        send("Tab", "Tab", "\t", 0x09, mods);
        return 1;
    case 10: case 13:
        send("Enter", "Enter", "\r", 0x0d, mods);
        return 1;
    case 27:
        send("Escape", "Escape", nullptr, 0x1b, mods);
        return 1;
    }

    // Control with a letter arrives as the letter's control character.
    if (key >= 1 && key <= 26) {
        char letter[2] = { (char)('a' + key - 1), 0 };
        char dom[5] = { 'K', 'e', 'y', (char)('A' + key - 1), 0 };
        send(letter, dom, nullptr, 'A' + key - 1, mods | 4);
        return 1;
    }
    if (key < 32)
        return 0;

    // A character: what it types, in UTF-8 - a byte above 127 is Latin-1's.
    char text[3] = { 0, 0, 0 };
    if (key < 128)
        text[0] = (char)key;
    else {
        text[0] = (char)(0xc0 | (key >> 6));
        text[1] = (char)(0x80 | (key & 0x3f));
    }

    char dom[16] = "";
    int virtualKey = 0;
    if (key == ' ') {
        strcpy(dom, "Space");
        virtualKey = 0x20;
    } else if ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z')) {
        int upper = key & ~0x20;
        strcpy(dom, "KeyA");
        dom[3] = (char)upper;
        virtualKey = upper;
        if (key == upper)
            mods |= 1;
    } else if (key >= '0' && key <= '9') {
        strcpy(dom, "Digit0");
        dom[5] = (char)key;
        virtualKey = key;
    } else if (const char* at = strchr(shiftedDigits, key); at && key) {
        strcpy(dom, "Digit0");
        dom[5] = (char)('0' + (at - shiftedDigits));
        virtualKey = dom[5];
        mods |= 1;
    } else {
        for (auto& p : punctuation) {
            if (key == p.plain || key == p.shifted) {
                strcpy(dom, p.dom);
                virtualKey = p.virtualKey;
                if (key == p.shifted)
                    mods |= 1;
                break;
            }
        }
    }
    send(text, dom[0] ? dom : "Unidentified", text, virtualKey, mods);
    return 1;
}

extern "C" void jetski_focus(int focused)
{
    if (browser.tab)
        WKViewSetFocus(browser.tab->view, focused);
}
