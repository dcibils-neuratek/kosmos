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
#include <WebKit/WKRetainPtr.h>
#include <WebKit/WKString.h>
#include <WebKit/WKURL.h>
#include <WebKit/WKView.h>
#include <WebKit/WKViewClient.h>
#include <string.h>
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

WKContextRef context()
{
    static WKContextRef shared = [] {
        auto configuration = adoptWK(WKContextConfigurationCreate());
        return WKContextCreateWithConfiguration(configuration.get());
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

    static WTF::RunLoop::Timer slice(WTF::RunLoop::mainSingleton(), "jetski slice"_s, [] {
        browser.stop();
    });
    browser.slice = &slice;
    return 0;
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
