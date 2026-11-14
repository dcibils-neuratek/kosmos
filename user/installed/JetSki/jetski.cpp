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

#include <WebKit/WKContext.h>
#include <WebKit/WKContextConfigurationRef.h>
#include <WebKit/WKErrorRef.h>
#include <WebKit/WKPage.h>
#include <WebKit/WKPageConfigurationRef.h>
#include <WebKit/WKPageKosmos.h>
#include <WebKit/WKPageNavigationClient.h>
#include <WebKit/WKPageUIClient.h>
#include <WebKit/WKRetainPtr.h>
#include <WebKit/WKString.h>
#include <WebKit/WKURL.h>
#include <WebKit/WKView.h>
#include <WebKit/WKViewClient.h>
#include <WebKit/WKWebsiteDataStoreConfigurationRef.h>
#include <WebKit/WKWebsiteDataStoreRef.h>
#include <stdlib.h>
#include <string.h>
#include <wtf/MonotonicTime.h>
#include <wtf/RunLoop.h>
#include <wtf/Seconds.h>

extern "C" int kosmos_webkit_role(unsigned long role);

/* A web or network process arrives here (`user/init/main.c`). */
extern "C" int kosmos_project_role(unsigned long role)
{
    return kosmos_webkit_role(role);
}

namespace {

struct Browser {
    WKViewRef view = nullptr;
    WKPageRef page = nullptr;
    unsigned width = 0, height = 0;
    unsigned happened = 0;
    int error = 0;
    bool loading = false;
    bool running = false;           // inside `jetski_pump`'s run loop
    WTF::RunLoop::Timer* slice = nullptr;

    void stop()
    {
        if (running) {
            running = false;
            WTF::RunLoop::mainSingleton().stop();
        }
    }
};

Browser browser;

void didStart(WKPageRef, WKNavigationRef, WKTypeRef, const void*)
{
    browser.happened |= JETSKI_STARTED;
    browser.loading = true;
}

void didCommit(WKPageRef, WKNavigationRef, WKTypeRef, const void*)
{
    browser.happened |= JETSKI_COMMITTED;
}

void didFinish(WKPageRef, WKNavigationRef, WKTypeRef, const void*)
{
    browser.happened |= JETSKI_FINISHED;
    browser.loading = false;
    browser.stop();
}

void didFail(WKPageRef, WKNavigationRef, WKErrorRef error, WKTypeRef, const void*)
{
    browser.happened |= JETSKI_FAILED;
    browser.error = error ? WKErrorGetErrorCode(error) : 0;
    browser.loading = false;
    browser.stop();
}

void didCrash(WKPageRef, const void*)
{
    browser.happened |= JETSKI_CRASHED;
    browser.loading = false;
    browser.stop();
}

void needsDisplay(WKViewRef, WKRect, const void*)
{
    browser.happened |= JETSKI_PICTURE;
    browser.stop();
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

} // namespace

extern "C" int jetski_open(unsigned width, unsigned height)
{
    if (browser.view || !width || !height)
        return browser.view ? 0 : 1;

    auto configuration = adoptWK(WKPageConfigurationCreate());
    WKPageConfigurationSetContext(configuration.get(), context());
    WKPageConfigurationSetWebsiteDataStore(configuration.get(), dataStore());
    browser.view = WKViewCreate(configuration.get());
    browser.width = width;
    browser.height = height;
    WKViewSetSize(browser.view, WKSizeMake(width, height));
    WKViewSetVisible(browser.view, true);
    WKViewSetActive(browser.view, true);
    WKViewSetFocus(browser.view, true);

    static WKViewClientV0 viewClient { };
    viewClient.base = { 0, nullptr };
    viewClient.setViewNeedsDisplay = needsDisplay;
    WKViewSetViewClient(browser.view, &viewClient.base);

    browser.page = WKViewGetPage(browser.view);
    static WKPageNavigationClientV0 navigation { };
    navigation.base = { 0, nullptr };
    navigation.didStartProvisionalNavigation = didStart;
    navigation.didCommitNavigation = didCommit;
    navigation.didFinishNavigation = didFinish;
    navigation.didFailNavigation = didFail;
    navigation.didFailProvisionalNavigation = didFail;
    navigation.webProcessDidCrash = didCrash;
    WKPageSetPageNavigationClient(browser.page, &navigation.base);

    static WKPageUIClientV0 ui { };
    ui.base = { 0, nullptr };
    ui.getWindowFrame = getWindowFrame;
    WKPageSetPageUIClient(browser.page, &ui.base);

    static WTF::RunLoop::Timer slice(WTF::RunLoop::mainSingleton(), "jetski slice"_s, [] {
        browser.stop();
    });
    browser.slice = &slice;
    return 0;
}

extern "C" void jetski_frame(int x, int y, unsigned width, unsigned height)
{
    windowFrame = WKRectMake(x, y, width, height);
}

extern "C" void jetski_resize(unsigned width, unsigned height)
{
    if (!browser.view || !width || !height)
        return;
    browser.width = width;
    browser.height = height;
    WKViewSetSize(browser.view, WKSizeMake(width, height));
}

extern "C" void jetski_load(const char* address)
{
    if (!browser.page || !address)
        return;
    auto url = adoptWK(WKURLCreateWithUTF8CString(address));
    WKPageLoadURL(browser.page, url.get());
}

extern "C" void jetski_back(void)
{
    if (browser.page && WKPageCanGoBack(browser.page))
        WKPageGoBack(browser.page);
}

extern "C" void jetski_forward(void)
{
    if (browser.page && WKPageCanGoForward(browser.page))
        WKPageGoForward(browser.page);
}

extern "C" void jetski_reload(void)
{
    if (browser.page)
        WKPageReload(browser.page);
}

extern "C" void jetski_stop(void)
{
    if (browser.page)
        WKPageStopLoading(browser.page);
}

extern "C" void jetski_pump(unsigned ms, struct jetski_state* out)
{
    memset(out, 0, sizeof *out);
    if (!browser.page)
        return;

    // Something already waiting to be said is said now, without a slice.
    if (!browser.happened && ms) {
        browser.running = true;
        browser.slice->startOneShot(WTF::Seconds::fromMilliseconds(ms));
        WTF::RunLoop::run();
        browser.running = false;
        browser.slice->stop();
    }

    out->happened = browser.happened;
    out->error = browser.error;
    browser.happened = 0;
    browser.error = 0;
    out->loading = browser.loading;
    out->progress = WKPageGetEstimatedProgress(browser.page);
    out->can_back = WKPageCanGoBack(browser.page);
    out->can_forward = WKPageCanGoForward(browser.page);

    auto title = adoptWK(WKPageCopyTitle(browser.page));
    copy(title.get(), out->title, sizeof out->title);

    auto url = adoptWK(WKPageCopyActiveURL(browser.page));
    if (url) {
        auto string = adoptWK(WKURLCopyString(url.get()));
        copy(string.get(), out->address, sizeof out->address);
    }
}

extern "C" void jetski_paint(uint32_t* pixels, unsigned width, unsigned height, unsigned pitch)
{
    if (!browser.page)
        return;
    WKPagePaintKosmos(browser.page, pixels, width, height, pitch, WKRectMake(0, 0, width, height));
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
    if (!browser.page)
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
        WKPageMouseKosmos(browser.page, kWKKosmosMouseDown, button, pointer.held, x, y, pointer.clicks, modifiersOf(mods));
    } else if (action == JETSKI_RELEASE) {
        pointer.held &= ~heldBit(button);
        WKPageMouseKosmos(browser.page, kWKKosmosMouseUp, button, pointer.held, x, y, pointer.clicks, modifiersOf(mods));
    } else {
        // A move with a button held is a drag of that button, as WebKit has it.
        int moving = pointer.held & 1 ? JETSKI_LEFT : pointer.held & 4 ? JETSKI_MIDDLE : pointer.held & 2 ? JETSKI_RIGHT : JETSKI_NONE;
        WKPageMouseKosmos(browser.page, kWKKosmosMouseMove, moving, pointer.held, x, y, 0, modifiersOf(mods));
    }
}

//
// **The wheel**: a notch is three of WebKit's lines, 40 pixels each, as
// the kit scrolls three rows a notch (`ui.WHEEL_ROWS`). Shift turns it
// sideways, as it does on every desktop.
//
extern "C" void jetski_wheel(int x, int y, int notches, unsigned mods)
{
    if (!browser.page || !notches)
        return;
    float ticks = notches, pixels = notches * 3 * 40;
    if (mods & 1)
        WKPageWheelKosmos(browser.page, x, y, pixels, 0, ticks, 0, modifiersOf(mods & ~1u));
    else
        WKPageWheelKosmos(browser.page, x, y, 0, pixels, 0, ticks, modifiersOf(mods));
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
    WKPageKeyKosmos(browser.page, true, key, dom, text, virtualKey, modifiersOf(mods));
    WKPageKeyKosmos(browser.page, false, key, dom, nullptr, virtualKey, modifiersOf(mods));
}

}

extern "C" int jetski_key(int c)
{
    if (!browser.page)
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
    if (browser.view)
        WKViewSetFocus(browser.view, focused);
}
