/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **W7a's WebKit2 test** (`webkit2test.h`): WebKit's MiniBrowser shape
 * with no window. The suite's image is a UI process; the web and network
 * processes are the same image started again in their roles, which
 * `kosmos_project_role` sends to WebKit's entry for each.
 *
 * A page is a view of PlayStation's (which without WPE is nothing of
 * PlayStation's) at the surface's size, loaded from a string and run on
 * the main run loop until its navigation finishes and a picture has
 * arrived after it. The picture is then painted by `WKPagePaintKosmos`.
 *
 * In `WEBKIT2TEST_URL` the page is an address, and the network process
 * fetches it and everything it refers to, through curl (W7b).
 *
 * The control (`WEBKIT2TEST_KILL`): the web process is ended as soon as
 * the navigation commits. The UI process must be told, say so, and load
 * the page again - its string, a new web process has never seen it - in a
 * new web process, and that must paint.
 */

#include "webkit2test.h"

#include <WebKit/WKContext.h>
#include <WebKit/WKContextConfigurationRef.h>
#include <WebKit/WKErrorRef.h>
#include <WebKit/WKPage.h>
#include <WebKit/WKPageConfigurationRef.h>
#include <WebKit/WKPageKosmos.h>
#include <WebKit/WKPageNavigationClient.h>
#include <WebKit/WKPagePrivate.h>
#include <WebKit/WKRetainPtr.h>
#include <WebKit/WKString.h>
#include <WebKit/WKURL.h>
#include <WebKit/WKView.h>
#include <WebKit/WKViewClient.h>
#include <kosmos.h>
#include <stdio.h>
#include <wtf/RunLoop.h>
#include <wtf/Seconds.h>

extern "C" int kosmos_webkit_role(unsigned long role);

/* A web or network process arrives here (`user/init/main.c`). */
extern "C" int kosmos_project_role(unsigned long role)
{
    return kosmos_webkit_role(role);
}

namespace {

struct Load {
    webkit2test_say_fn say;
    void* ctx;
    int mode;
    const char* html;
    bool finished = false;
    bool pictured = false;      // finished, a picture since, and then quiet
    bool seen = false;          // a picture arrived since the page committed
    WTF::RunLoop::Timer* settle = nullptr;
    bool crashed = false;
    bool killed = false;
    bool failed = false;
    bool timedOut = false;
    WKPageRef page = nullptr;

    void line(const char* format, ...) __attribute__((format(printf, 2, 3)))
    {
        char text[256];
        va_list args;
        va_start(args, format);
        vsnprintf(text, sizeof text, format, args);
        va_end(args);
        say(text, ctx);
    }

    bool done() const { return failed || timedOut || (finished && pictured); }
    void stopIfDone() const
    {
        if (done())
            WTF::RunLoop::mainSingleton().stop();
    }
};

Load& of(const void* info) { return *const_cast<Load*>(static_cast<const Load*>(info)); }

void didStart(WKPageRef page, WKNavigationRef, WKTypeRef, const void* info)
{
    of(info).line("started in %d", static_cast<int>(WKPageGetProcessIdentifier(page)));
}

// **A page is pictured when it has a picture and has stopped changing.** A
// small page paints after its load event, and that picture was the signal;
// a large one - Wikipedia - paints as it arrives, and its last picture can
// come before the finish, after which nothing more ever does. So: a picture
// since the commit, then a quiet second after the finish with no new one.
constexpr WTF::Seconds quiet { 1 };

void settled(Load& load)
{
    if (load.finished && load.seen && !load.pictured) {
        load.pictured = true;
        load.line("pictured");
    }
    load.stopIfDone();
}

void didCommit(WKPageRef page, WKNavigationRef, WKTypeRef, const void* info)
{
    auto& load = of(info);
    load.seen = false;                      // pictures of this page, from now
    load.line("committed in %d", static_cast<int>(WKPageGetProcessIdentifier(page)));
    if (load.mode == WEBKIT2TEST_KILL && !load.killed) {
        load.killed = true;
        load.line("ending the web process");
        kosmos_kill(static_cast<unsigned long>(WKPageGetProcessIdentifier(page)));
    }
}

void didFinish(WKPageRef, WKNavigationRef, WKTypeRef, const void* info)
{
    auto& load = of(info);
    load.finished = true;
    load.line("finished");
    if (load.settle)
        load.settle->startOneShot(quiet);
}

void didFail(WKPageRef, WKNavigationRef, WKErrorRef error, WKTypeRef, const void* info)
{
    auto& load = of(info);
    load.failed = true;
    // The code is curl's for a failure of the network - 60 for a peer whose
    // certificate was not trusted - so a refusal can be told from the rest.
    load.line("failed, error %d", error ? WKErrorGetErrorCode(error) : 0);
    load.stopIfDone();
}

void didCrash(WKPageRef page, const void* info)
{
    auto& load = of(info);
    load.crashed = true;
    load.line("the web process ended");
    if (!load.killed) {
        load.failed = true;
        load.stopIfDone();
        return;
    }
    load.finished = false;
    load.pictured = false;
    load.seen = false;
    load.line("loading again");
    // The page was given as a string, which a new web process has never
    // seen: it is given again, as a browser loads the address again.
    auto string = adoptWK(WKStringCreateWithUTF8CString(load.html));
    auto base = adoptWK(WKURLCreateWithUTF8CString("about:blank"));
    WKPageLoadHTMLString(page, string.get(), base.get());
}

void needsDisplay(WKViewRef, WKRect, const void* info)
{
    auto& load = of(info);
    load.seen = true;
    if (load.finished && !load.pictured && load.settle)
        load.settle->startOneShot(quiet);
}

WKContextRef context()
{
    static WKContextRef shared = [] {
        auto configuration = adoptWK(WKContextConfigurationCreate());
        return WKContextCreateWithConfiguration(configuration.get());
    }();
    return shared;
}

} // namespace

extern "C" int webkit2test_load(const char* html, uint32_t* pixels, unsigned width, unsigned height,
    unsigned pitch, unsigned ms, int mode, webkit2test_say_fn say, void* ctx)
{
    Load load { say, ctx, mode, html };

    auto configuration = adoptWK(WKPageConfigurationCreate());
    WKPageConfigurationSetContext(configuration.get(), context());
    auto view = adoptWK(WKViewCreate(configuration.get()));
    WKViewSetSize(view.get(), WKSizeMake(width, height));
    WKViewSetVisible(view.get(), true);
    WKViewSetActive(view.get(), true);
    WKViewSetFocus(view.get(), true);

    WKViewClientV0 viewClient { };
    viewClient.base = { 0, &load };
    viewClient.setViewNeedsDisplay = needsDisplay;
    WKViewSetViewClient(view.get(), &viewClient.base);

    load.page = WKViewGetPage(view.get());
    WKPageNavigationClientV0 navigation { };
    navigation.base = { 0, &load };
    navigation.didStartProvisionalNavigation = didStart;
    navigation.didCommitNavigation = didCommit;
    navigation.didFinishNavigation = didFinish;
    navigation.didFailNavigation = didFail;
    navigation.didFailProvisionalNavigation = didFail;
    navigation.webProcessDidCrash = didCrash;
    WKPageSetPageNavigationClient(load.page, &navigation.base);

    WTF::RunLoop::Timer settle(WTF::RunLoop::mainSingleton(), "webkit2test settle"_s, [&load] {
        settled(load);
    });
    load.settle = &settle;

    if (mode == WEBKIT2TEST_URL) {
        auto url = adoptWK(WKURLCreateWithUTF8CString(html));
        WKPageLoadURL(load.page, url.get());
    } else {
        auto string = adoptWK(WKStringCreateWithUTF8CString(html));
        auto base = adoptWK(WKURLCreateWithUTF8CString("about:blank"));
        WKPageLoadHTMLString(load.page, string.get(), base.get());
    }

    WTF::RunLoop::Timer deadline(WTF::RunLoop::mainSingleton(), "webkit2test deadline"_s, [&load] {
        load.timedOut = true;
        load.line("no picture in time (finished %d, pictured %d)", load.finished, load.pictured);
        load.stopIfDone();
    });
    deadline.startOneShot(WTF::Seconds::fromMilliseconds(ms));
    if (!load.done())
        WTF::RunLoop::run();
    deadline.stop();

    WKPageNavigationClientV0 none { };
    WKPageSetPageNavigationClient(load.page, &none.base);
    WKViewSetViewClient(view.get(), nullptr);

    if (!(load.finished && load.pictured))
        return 1;
    WKPagePaintKosmos(load.page, pixels, width, height, pitch, WKRectMake(0, 0, width, height));
    load.line("painted");
    WKPageClose(load.page);
    return 0;
}
