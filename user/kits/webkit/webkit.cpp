/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The WebKit Kit** (`webkit.h`, `docs/mailweb.md` W2): WebKit2's UI
 * process as views - a page each, of a size, with its options - whose run
 * loop is run a slice at a time between the window's own events, and whose
 * pictures are painted into the caller's surface when they change. JetSki
 * holds one a tab; Mail will hold one for its message pane.
 *
 * The web and network processes are this image again, in their roles
 * (`kosmos_project_role`), as `webkit2test`'s are.
 *
 * **A slice ends early when a view shown has news**: WebKit says the view
 * needs painting, the loop is stopped, and the window paints it at once -
 * so a page appears as it arrives rather than at the slice's end.
 *
 * Until W2 this was `jetski.cpp`, one page with tabs inside it; the tabs
 * are JetSki's now, and a view knows nothing of them.
 */

#include "webkit.h"

#include <kosmos.h>                 // kosmos_wait, struct process_death
#include <atomic>
#include <stdio.h>
#include <string.h>

#include <WebKit/WKContext.h>
#include <WebKit/WKContextConfigurationRef.h>
#include <WebKit/WKErrorRef.h>
#include <WebKit/WKFrameInfoRef.h>
#include <WebKit/WKFramePolicyListener.h>
#include <WebKit/WKInspector.h>
#include <WebKit/WKNavigationActionRef.h>
#include <WebKit/WKPage.h>
#include <WebKit/WKPageConfigurationRef.h>
#include <WebKit/WKPageKosmos.h>
#include <WebKit/WKPageLoadTypes.h>
#include <WebKit/WKPageNavigationClient.h>
#include <WebKit/WKPageResourceLoadClient.h>
#include <WebKit/WKPagePrivate.h>
#include <WebKit/WKPageUIClient.h>
#include <WebKit/WKPreferencesRef.h>
#include <WebKit/WKRetainPtr.h>
#include <WebKit/WKString.h>
#include <WebKit/WKURL.h>
#include <WebKit/WKArray.h>
#include <WebKit/WKAuthenticationChallenge.h>
#include <WebKit/WKAuthenticationDecisionListener.h>
#include <WebKit/WKCredential.h>
#include <WebKit/WKProtectionSpace.h>
#include <WebKit/WKProtectionSpaceCurl.h>
#include <WebKit/WKUserContentControllerRef.h>
#include <WebKit/WKUserContentExtensionStoreRef.h>
#include <WebKit/WKURLRequest.h>
#include <WebKit/WKURLResponse.h>
#include <WebKit/WKView.h>
#include <WebKit/WKViewClient.h>
#include <WebKit/WKWebsiteDataStoreConfigurationRef.h>
#include <WebKit/WKWebsiteDataStoreRef.h>
#include <WebKit/WKWebsitePolicies.h>
#include <stdlib.h>
#include <wtf/FileSystem.h>
#include <wtf/text/MakeString.h>
#include <zlib.h>
#include <wtf/MonotonicTime.h>
#include <wtf/RunLoop.h>
#include <wtf/Seconds.h>
#include <wtf/Vector.h>
#include <memory>
#include <algorithm>

extern "C" int kosmos_webkit_role(unsigned long role);

/* A web or network process arrives here (`user/init/main.c`). */
extern "C" int kosmos_project_role(unsigned long role)
{
    return kosmos_webkit_role(role);
}

namespace {

/*
 * **A view**: WebKit's view and its page, a web process of its own, its
 * options, and what its page has said since it was last asked. Shown or
 * dark is the caller's to say; a dark one goes on unseen and inactive, as
 * a browser's background tab does.
 */
/*
 * **A page's load, traced** (Diego, 15 November 2026: "i find the jetski
 * browser really slow to use and load pages", "start measuring the page
 * load on the m700"; a page on the M700 held at 90%). WebKit tells the
 * page's owner of every request its page makes - sent, answered, ended -
 * through the resource load client, and with `webkit_trace` on, each is
 * kept from the moment it is sent and said in one line when it ends: when
 * it began after the page was asked for, its status, its first byte, its
 * end. While a page loads, what has not ended is said every five seconds;
 * that list is what holds a page at 90%. Off, nothing is kept or said.
 */
struct Load {
    char url[200];
    WTF::MonotonicTime sent, answered;
    int status = 0;
    bool answeredYet = false;
};

bool tracing = false;

/*
 * **Sites allowed** (`adblock.md` B4, Diego: "i want a button that i can
 * press and disable ad blocking and else on a per website basis"): hosts
 * whose pages load with WebKit's content blockers off - Safari's own
 * per-navigation switch, website policies handed with the decision. A
 * host on the list allows itself and every host under it.
 */
Vector<String> allowedSites;

bool allowedSite(const char* host)
{
    String name = String::fromUTF8(host);
    for (auto& site : allowedSites) {
        if (name == site)
            return true;
        if (name.length() > site.length() && name.endsWith(site)
            && name[name.length() - site.length() - 1] == '.')
            return true;
    }
    return false;
}

struct View {
    int id = 0;
    WKViewRef view = nullptr;
    WKPageRef page = nullptr;
    struct webkit_options options { 1, 1 };
    bool shown = false;
    unsigned width = 0, height = 0; // its size, which WebKit's C API does not say
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
    char held[2048] { };            // a load waiting for the rule lists
    unsigned blocked = 0;           // what the rule lists kept out of this page
    unsigned cookiesBlocked = 0;
    // **A certificate refused, or taken anyway** (`docs/padlock.html`):
    // the server, `host:port`, whose certificate this view takes as it
    // is, after Open anyway; and while a page is asked for, the server
    // refused and why -
    // which becomes `refused` when the page's own load fails for it.
    char anyway[256] { };
    char refusedHost[256] { };
    int refusedError = 0;
    char refusedReason[256] { };
    bool refused = false;
    char refusedAddress[2048] { };
    // **What was kept out, by whom** (`adblock.md` B4, the shield's *Most
    // blocked on this page*): the host each blocked request was for, and
    // how many; and whether this page was loaded with blocking off, its
    // site being one a person allowed.
    Vector<struct webkit_kept> kept;
    bool unblocked = false;
    WKViewRef inspector = nullptr;  // its Web Inspector's view, while docked
    bool toInspector = false;       // input goes to the inspector's page

    // Its page's load, while traced (`webkit_trace`): the requests not yet
    // ended, and how the ones that did went.
    Vector<struct Load> loads;
    WTF::MonotonicTime traceFrom, lastReport;
    unsigned loadsDone = 0, loadsFailed = 0;

    // Its frames, while traced (*A frame's time, traced*): a turn of the
    // wheel to the picture it brought, and the page drawn into the window.
    struct Times {
        unsigned count = 0;
        double sum = 0, most = 0;
        void add(double ms) { count++; sum += ms; most = std::max(most, ms); }
    } toPicture, drawn, toSlice;
    unsigned pictures = 0, unsliced = 0;
    bool wheelWaiting = false, wheelSliced = false;
    WTF::MonotonicTime wheelAt, firstFrame, lastFrame;
};

/*
 * **The children, collected** (Diego, 14 November 2026: "yes fix the
 * zombie processes too"). The web and network processes are this
 * process's children - `ProcessLauncherKosmos.cpp` spawns them - and one
 * that ended stayed in Nebula's table until this process did, because
 * nothing waited for it. Every pump collects whoever has ended, without
 * waiting, and keeps the last few deaths by id: a view whose page stopped
 * asks for its own.
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
            fprintf(stderr, "webkit: process %ld ended: %s at %#lx, touching %#lx, thread %u\n",
                d.pid, d.death.name, static_cast<unsigned long>(d.death.pc),
                static_cast<unsigned long>(d.death.addr), d.death.thread);
        else
            fprintf(stderr, "webkit: process %ld ended, code %ld\n", d.pid, d.code);
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

struct Views {
    Vector<std::unique_ptr<View>> all;
    int nextId = 1;
    int opened = 0, opener = 0;     // a view a page opened, not yet told
    int closing = 0;                // a view whose page asked to be closed
    unsigned inspectorWidth = 0, inspectorHeight = 0;
    bool running = false;           // inside `webkit_pump`'s run loop
    WTF::RunLoop::Timer* slice = nullptr;

    void stop()
    {
        if (running) {
            running = false;
            WTF::RunLoop::mainSingleton().stop();
        }
    }

    View* find(int id)
    {
        for (auto& view : all) {
            if (view->id == id)
                return view.get();
        }
        return nullptr;
    }
};

Views views;

View* viewOf(const void* clientInfo)
{
    return const_cast<View*>(static_cast<const View*>(clientInfo));
}

// What a view shown has is told at once; a dark one waits its turn, unless
// it finished or ended, which the caller draws (a tab's title, its spinner).
void told(View* view, unsigned what)
{
    view->happened |= what;
    if (view->shown || what & (WEBKIT_FINISHED | WEBKIT_FAILED | WEBKIT_CRASHED))
        views.stop();
}

void copy(WKStringRef, char* into, size_t room);

void didStart(WKPageRef, WKNavigationRef, WKTypeRef, const void* info)
{
    auto* view = viewOf(info);
    view->crashed = false;
    view->loading = true;
    view->asked = false;
    view->blocked = view->cookiesBlocked = 0;
    view->kept.clear();
    view->refused = false;
    view->refusedHost[0] = '\0';
    told(view, WEBKIT_STARTED);
}

// **A URL's server, as `host:port`**, into `into` - the port said even
// where the URL leaves it out, 443 for https and 80 for http - so a
// certificate's server and a page's compare as one string. "" for none.
void serverOf(WKURLRef url, char* into, size_t room)
{
    into[0] = '\0';
    if (!url)
        return;
    auto host = adoptWK(WKURLCopyHostName(url));
    auto scheme = adoptWK(WKURLCopyScheme(url));
    auto whole = adoptWK(WKURLCopyString(url));
    char name[256], how[16], text[2048];
    copy(host.get(), name, sizeof name);
    copy(scheme.get(), how, sizeof how);
    copy(whole.get(), text, sizeof text);
    if (!name[0])
        return;
    // The port, if the address says one: after the host, before the path.
    long port = !strcmp(how, "https") ? 443 : 80;
    const char* at = strstr(text, "://");
    if (at) {
        at += 3;
        const char* end = at + strcspn(at, "/?#");
        // After an IPv6 address's bracket, whose own colons are not one.
        const char* from = at;
        for (const char* c = at; c < end; c++)
            if (*c == ']')
                from = c;
        const char* colon = nullptr;
        for (const char* c = from; c < end; c++)
            if (*c == ':')
                colon = c;
        if (colon)
            port = strtol(colon + 1, nullptr, 10);
    }
    snprintf(into, room, "%s:%ld", name, port);
}

/*
 * **A certificate that did not check out** (`docs/padlock.html`). curl
 * refused it, and WebKit's network process asks the page's owner whether
 * to go on: the host this view opens anyway is let through - a credential
 * of no account, which is how the curl port is told to load again without
 * checking (`NetworkDataTaskCurl::restartWithCredential`) - and any other is
 * left to WebKit, which refuses it, the reason kept for the page JetSki
 * shows instead. A challenge of another kind - a password - is WebKit's as
 * it was before the kit answered any.
 */
void challenged(WKPageRef, WKAuthenticationChallengeRef challenge, const void* info)
{
    auto* view = viewOf(info);
    auto space = WKAuthenticationChallengeGetProtectionSpace(challenge);
    auto listener = WKAuthenticationChallengeGetDecisionListener(challenge);

    if (WKProtectionSpaceGetAuthenticationScheme(space) != kWKProtectionSpaceAuthenticationSchemeServerTrustEvaluationRequested) {
        WKAuthenticationDecisionListenerPerformDefaultHandlingKosmos(listener);
        return;
    }

    char name[200], host[256];
    auto named = adoptWK(WKProtectionSpaceCopyHost(space));
    copy(named.get(), name, sizeof name);
    snprintf(host, sizeof host, "%s:%d", name, WKProtectionSpaceGetPort(space));

    if (view->anyway[0] && !strcmp(host, view->anyway)) {
        fprintf(stderr, "webkit: view %d takes %s's certificate as it is\n", view->id, host);
        auto user = adoptWK(WKStringCreateWithUTF8CString("anyway"));
        auto password = adoptWK(WKStringCreateWithUTF8CString(""));
        auto credential = adoptWK(WKCredentialCreate(user.get(), password.get(), kWKCredentialPersistenceNone));
        WKAuthenticationDecisionListenerUseCredential(listener, credential.get());
        return;
    }

    memcpy(view->refusedHost, host, sizeof view->refusedHost);
    view->refusedError = WKProtectionSpaceGetCertificateVerificationError(space);
    auto reason = adoptWK(WKProtectionSpaceCopyCertificateVerificationErrorDescription(space));
    view->refusedReason[0] = '\0';
    if (reason)
        copy(reason.get(), view->refusedReason, sizeof view->refusedReason);
    fprintf(stderr, "webkit: view %d refused %s's certificate: %d, %s\n",
        view->id, host, view->refusedError, view->refusedReason);
    WKAuthenticationDecisionListenerPerformDefaultHandlingKosmos(listener);
}

/*
 * **A link, the moment it is followed** (Diego, 14 November 2026: "right
 * now i click some links and nothing happens"). WebKit says a navigation
 * began only once it is decided and, for another site, a web process has
 * been started for it - on the M700 a second or more after the click, with
 * nothing on screen. Asking is the first thing that happens, so the page is
 * loading from here: for the page itself, not a frame inside it.
 *
 * **A view that does not follow** (`options.follow` 0, Mail's message
 * pane): a link clicked or a form sent is reported - `WEBKIT_ASKED` and
 * `going` - and goes no further, so the view stays on what it was given.
 * What the caller loads itself, and going back or forward, still happen.
 */
void decidePolicy(WKPageRef, WKNavigationActionRef action, WKFramePolicyListenerRef listener, WKTypeRef, const void* info)
{
    if (WKNavigationActionShouldPerformDownload(action)) {
        WKFramePolicyListenerDownload(listener);
        return;
    }

    auto* view = viewOf(info);
    auto frame = adoptWK(WKNavigationActionCopyTargetFrameInfo(action));
    auto type = WKNavigationActionGetNavigationType(action);
    bool followed = type == kWKFrameNavigationTypeLinkClicked || type == kWKFrameNavigationTypeFormSubmitted;
    bool mainFrame = !frame || WKFrameInfoGetIsMainFrame(frame.get());

    if (mainFrame || (followed && !view->options.follow)) {
        char going[2048] = "";
        auto request = adoptWK(WKNavigationActionCopyRequest(action));
        auto url = request ? adoptWK(WKURLRequestCopyURL(request.get())) : nullptr;
        if (url) {
            auto string = adoptWK(WKURLCopyString(url.get()));
            copy(string.get(), going, sizeof going);
        }
        memcpy(view->going, going, sizeof view->going);
        if (followed && !view->options.follow) {
            told(view, WEBKIT_ASKED);
            WKFramePolicyListenerIgnore(listener);
            return;
        }
        view->asked = true;
        view->askedAt = WTF::MonotonicTime::now();
        if (mainFrame) {
            view->traceFrom = view->lastReport = view->askedAt;
            view->loads.clear();
            view->loadsDone = view->loadsFailed = 0;
        }
        told(view, WEBKIT_ASKED);
        // A site a person allowed: its page, and everything in it, with the
        // rule lists off.
        if (mainFrame && url) {
            auto host = adoptWK(WKURLCopyHostName(url.get()));
            char name[256];
            copy(host.get(), name, sizeof name);
            if (allowedSite(name)) {
                auto policies = adoptWK(WKWebsitePoliciesCreate());
                WKWebsitePoliciesSetContentBlockersEnabled(policies.get(), false);
                fprintf(stderr, "webkit: view %d loads %s with blocking off\n", view->id, name);
                view->unblocked = true;
                WKFramePolicyListenerUseWithPolicies(listener, policies.get());
                return;
            }
            view->unblocked = false;
        }
    }
    WKFramePolicyListenerUse(listener);
}

// A link to a place on the same page, `#notes`: asked, and never loaded.
void didSameDocument(WKPageRef, WKNavigationRef, WKSameDocumentNavigationType, WKTypeRef, const void* info)
{
    viewOf(info)->asked = false;
}

// What a rule list kept out of the page: patch 0033's two names.
// One more request kept out for `host`, on the view's list of them.
void keptFor(View* view, const char* host)
{
    for (auto& each : view->kept) {
        if (!strcmp(each.host, host)) {
            each.count++;
            return;
        }
    }
    struct webkit_kept one { };
    snprintf(one.host, sizeof one.host, "%s", host);
    one.count = 1;
    view->kept.append(one);
}

void ruleListActed(WKPageRef, WKURLRef url, WKArrayRef, WKArrayRef notifications, const void* info)
{
    auto* view = viewOf(info);
    for (size_t i = 0; i < WKArrayGetSize(notifications); i++) {
        auto said = static_cast<WKStringRef>(WKArrayGetItemAtIndex(notifications, i));
        if (WKStringIsEqualToUTF8CString(said, "kosmos:blocked")) {
            view->blocked++;
            char host[128] = "";
            if (url) {
                auto name = adoptWK(WKURLCopyHostName(url));
                copy(name.get(), host, sizeof host);
            }
            keptFor(view, host[0] ? host : "an address with no host");
        }
        else if (WKStringIsEqualToUTF8CString(said, "kosmos:cookies"))
            view->cookiesBlocked++;
    }
}

void didCommit(WKPageRef page, WKNavigationRef, WKTypeRef, const void* info)
{
    auto* view = viewOf(info);
    // A page of another host: Open anyway was for the one before.
    if (view->anyway[0]) {
        char host[256];
        auto url = adoptWK(WKPageCopyCommittedURL(page));
        serverOf(url.get(), host, sizeof host);
        if (strcmp(host, view->anyway)) {
            fprintf(stderr, "webkit: view %d left %s, its certificate checked again\n", view->id, view->anyway);
            view->anyway[0] = '\0';
        }
    }
    told(view, WEBKIT_COMMITTED);
}

void traceSummary(View*, const char* how);

void didFinish(WKPageRef, WKNavigationRef, WKTypeRef, const void* info)
{
    auto* view = viewOf(info);
    traceSummary(view, "finished");
    if (view->blocked || view->cookiesBlocked)
        fprintf(stderr, "webkit: view %d kept out %u requests, cookies of %u\n", view->id, view->blocked, view->cookiesBlocked);
    view->loading = false;
    view->asked = false;
    told(view, WEBKIT_FINISHED);
}

void didFail(WKPageRef, WKNavigationRef, WKErrorRef error, WKTypeRef, const void* info)
{
    auto* view = viewOf(info);
    traceSummary(view, "failed");
    view->error = error ? WKErrorGetErrorCode(error) : 0;
    // The page itself refused for its certificate - not a picture of
    // another host's in it, which only goes missing.
    if (view->refusedHost[0] && error) {
        char host[256];
        auto url = adoptWK(WKErrorCopyFailingURL(error));
        serverOf(url.get(), host, sizeof host);
        if (!strcmp(host, view->refusedHost)) {
            view->refused = true;
            auto string = url ? adoptWK(WKURLCopyString(url.get())) : nullptr;
            view->refusedAddress[0] = '\0';
            if (string)
                copy(string.get(), view->refusedAddress, sizeof view->refusedAddress);
        }
    }
    view->loading = false;
    view->asked = false;
    told(view, WEBKIT_FAILED);
}

void didCrash(WKPageRef page, const void* info)
{
    auto* view = viewOf(info);
    view->loading = false;
    view->crashed = true;
    view->crashedPid = view->pid;
    view->crashedAddress[0] = '\0';
    if (view->asked)
        memcpy(view->crashedAddress, view->going, sizeof view->crashedAddress);
    else if (auto url = adoptWK(WKPageCopyActiveURL(page))) {
        auto string = adoptWK(WKURLCopyString(url.get()));
        copy(string.get(), view->crashedAddress, sizeof view->crashedAddress);
    }
    fprintf(stderr, "webkit: view %d's page process %ld ended, at %s\n",
        view->id, view->crashedPid, view->crashedAddress);
    told(view, WEBKIT_CRASHED);
}

/*
 * **A frame's time, traced** (`webkit.md` *Fast*, step 1a; `testing.md`
 * 18.853): with `webkit_trace` on, how long a turn of the wheel took to
 * come back as a picture - the web process's painting, the hop and the UI
 * process taking it in, together - and how long the page took to draw into
 * the window, said as one line when a second passes with no frame.
 */
void frameSeen(View* view, WTF::MonotonicTime now)
{
    if (!view->pictures && !view->drawn.count)
        view->firstFrame = now;
    view->lastFrame = now;
}

void framesSaid(View* view, WTF::MonotonicTime now)
{
    if ((!view->pictures && !view->drawn.count) || now - view->lastFrame < 1_s)
        return;
    auto average = [](const View::Times& t) { return t.count ? t.sum / t.count : 0.0; };
    double takenAverage, takenMost, ringAverage, ringMost;
    unsigned taken = WKKosmosTakeIncorporated(&takenAverage, &takenMost, &ringAverage, &ringMost);
    fprintf(stderr, "webkit: frames view %d, %u pictures in %.0f ms: wheel to picture %.1f ms, most %.1f (%u); "
        "wheel to a slice %.1f, most %.1f (%u, %u passes without); "
        "read off the ring to taken %.1f, most %.1f; "
        "taken in %.1f ms, most %.1f (%u); drawn into the window %.1f ms, most %.1f (%u)\n",
        view->id, view->pictures, (view->lastFrame - view->firstFrame).milliseconds(),
        average(view->toPicture), view->toPicture.most, view->toPicture.count,
        average(view->toSlice), view->toSlice.most, view->toSlice.count, view->unsliced,
        ringAverage, ringMost,
        takenAverage, takenMost, taken,
        average(view->drawn), view->drawn.most, view->drawn.count);
    view->toPicture = { };
    view->toSlice = { };
    view->unsliced = 0;
    view->drawn = { };
    view->pictures = 0;
}

void needsDisplay(WKViewRef, WKRect, const void* info)
{
    auto* view = viewOf(info);
    if (tracing && view) {
        auto now = WTF::MonotonicTime::now();
        frameSeen(view, now);
        view->pictures++;
        if (view->wheelWaiting) {
            view->toPicture.add((now - view->wheelAt).milliseconds());
            view->wheelWaiting = false;
        }
    }
    told(view, WEBKIT_PICTURE);
}

// The inspector's view has a picture: its view's, said as the inspector's.
void inspectorNeedsDisplay(WKViewRef, WKRect, const void* info)
{
    told(viewOf(info), WEBKIT_INSPECTOR);
}

/*
 * **The inspector's view, from WebKit** (`WebInspectorUIProxyKosmos.cpp`):
 * the view whose page it inspects keeps it, sized as every inspector is;
 * a null view is the inspector gone.
 */
void inspectorView(WKPageRef inspected, WKViewRef inspector, void*)
{
    for (auto& view : views.all) {
        if (view->page != inspected)
            continue;
        if (view->inspector == inspector)
            return;
        if (view->inspector)
            WKRelease(view->inspector);
        view->inspector = inspector ? static_cast<WKViewRef>(const_cast<void*>(WKRetain(inspector))) : nullptr;
        if (inspector) {
            if (views.inspectorWidth && views.inspectorHeight)
                WKViewSetSize(inspector, WKSizeMake(views.inspectorWidth, views.inspectorHeight));
            WKViewClientV0 client { };
            client.base = { 0, view.get() };
            client.setViewNeedsDisplay = inspectorNeedsDisplay;
            WKViewSetViewClient(inspector, &client.base);
            WKViewSetVisible(inspector, view->shown);
            WKViewSetActive(inspector, view->shown);
        } else
            view->toInspector = false;
        fprintf(stderr, "webkit: view %d's inspector %s\n", view->id, inspector ? "docked" : "closed");
        told(view.get(), WEBKIT_INSPECTOR);
        return;
    }
}

/*
 * **Preferences, two of them** (`webkit.h`, a view's options): every view
 * that runs scripts is given one object, so turning the developer features
 * on or off is one call for all of them (`docs/webkit.html`: Settings,
 * *Show developer features*); every view that does not, the other, with
 * JavaScript off and never the developer features - a message is not
 * something to debug.
 */
WKPreferencesRef preferences(bool scripts)
{
    static WKPreferencesRef scripted = WKPreferencesCreate();
    static WKPreferencesRef plain = [] {
        auto made = WKPreferencesCreate();
        WKPreferencesSetJavaScriptEnabled(made, false);
        return made;
    }();
    return scripts ? scripted : plain;
}

/*
 * **The window views are in** (`webkit_frame`): what `window.outerWidth`,
 * `screenX` and the rest answer. Without it they are all nought, which no
 * browser a person uses says and which a site's robot check reads as a
 * browser with no window at all - the mark of one driven by a program.
 */
WKRect windowFrame;

// Every page's zoom, Settings' (`webkit_zoom`): 1 is 100%.
static double zoomFactor = 1;

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
 * **Privacy and Ad Blocking's rule lists** (`docs/adblock.md`, B1 and B2).
 * Every `.json` - or `.json.gz`, as the image carries EasyList and the
 * others, 1.6 MB where the JSON is 25 - in `/Kosmos/Filters` and in
 * `/Home/Settings/Filters`, the person's, is a WebKit content rule list.
 * Each is compiled into `/Home/Settings/Filters/Rules` on a WebKit work
 * queue, and added to the one content controller every view shares, so a
 * list applies to every page at once. A load asked for while any list is
 * still compiling is held, and made when the last one is ready: a page
 * that loaded before its lists would be the one page nothing was kept out
 * of.
 *
 * **Compiled once, not at every start.** A list's identifier carries a
 * hash of its file's bytes as they are kept - the gzip, 1.6 MB for all
 * three - so a list whose file has not changed is found already compiled
 * and used as it is, and nothing is inflated (Diego, 16 November:
 * "Unzipping on every launch should be expensive"). Only a list that is
 * new or has changed is inflated, compiled under its new name, and its old
 * compiled file removed.
 */
struct Rules {
    bool started = false;
    unsigned compiling = 0;
    WTF::MonotonicTime from;
};
Rules rules;

WKUserContentControllerRef contentController()
{
    static WKUserContentControllerRef shared = WKUserContentControllerCreate();
    return shared;
}

WKUserContentExtensionStoreRef ruleStore();

void releaseHeld();

// A list's file, as text: inflated when it is gzipped.
std::optional<Vector<uint8_t>> readList(const String& path)
{
    auto bytes = FileSystem::readEntireFile(path);
    if (!bytes || !path.endsWith(".gz"_s))
        return bytes;

    Vector<uint8_t> text;
    z_stream z { };
    if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK)
        return std::nullopt;
    z.next_in = const_cast<Bytef*>(bytes->span().data());
    z.avail_in = bytes->size();
    int status = Z_OK;
    while (status == Z_OK) {
        size_t at = text.size();
        text.grow(at + 1024 * 1024);
        z.next_out = text.mutableSpan().data() + at;
        z.avail_out = 1024 * 1024;
        status = inflate(&z, Z_NO_FLUSH);
        text.shrink(text.size() - z.avail_out);
    }
    inflateEnd(&z);
    if (status != Z_STREAM_END)
        return std::nullopt;
    return text;
}

// FNV-1a: a name for a list's bytes, so its compiled file is found again.
uint64_t hashOf(std::span<const uint8_t> bytes)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (auto b : bytes) {
        h ^= b;
        h *= 0x100000001b3ull;
    }
    return h;
}

struct Pending {
    char identifier[256];
    char base[200];
    String path;
};

void ruleListAdded(WKUserContentFilterRef filter, WKUserContentExtensionStoreResult result, void* context, bool compiled)
{
    auto* pending = static_cast<Pending*>(context);
    if (result == kWKUserContentExtensionStoreSuccess && filter) {
        WKUserContentControllerAddUserContentFilter(contentController(), filter);
        auto bytes = FileSystem::fileSize(makeString("/Home/Settings/Filters/Rules/ContentRuleList-"_s,
            String::fromUTF8(pending->identifier)));
        fprintf(stderr, "webkit: rules %s %s in %.0f ms, %llu KB\n", pending->base,
            compiled ? "ready" : "found compiled", (WTF::MonotonicTime::now() - rules.from).milliseconds(),
            static_cast<unsigned long long>(bytes.value_or(0) / 1024));
    } else
        fprintf(stderr, "webkit: rules %s refused (%u)\n", pending->base, result);
    delete pending;
    if (rules.compiling && !--rules.compiling)
        releaseHeld();
}

void ruleListCompiled(WKUserContentFilterRef filter, WKUserContentExtensionStoreResult result, void* context)
{
    ruleListAdded(filter, result, context, true);
}

void ruleListRemoved(WKUserContentFilterRef, WKUserContentExtensionStoreResult, void* context)
{
    free(context);
}

// Not compiled under this name yet: compiled now, and the same list's
// files of other bytes removed from the store.
void ruleListLookedUp(WKUserContentFilterRef filter, WKUserContentExtensionStoreResult result, void* context)
{
    auto* pending = static_cast<Pending*>(context);
    if (result == kWKUserContentExtensionStoreSuccess && filter) {
        ruleListAdded(filter, result, context, false);
        return;
    }

    // New, or changed: only now is it read as text.
    auto text = readList(pending->path);
    if (!text) {
        fprintf(stderr, "webkit: rules %s could not be read\n", pending->base);
        ruleListAdded(nullptr, kWKUserContentExtensionStoreCompileFailed, context, true);
        return;
    }
    text->append(0);
    auto json = adoptWK(WKStringCreateWithUTF8CString(reinterpret_cast<const char*>(text->span().data())));
    text = std::nullopt;

    String stem = makeString("ContentRuleList-"_s, String::fromUTF8(pending->base), '-');
    for (auto& name : FileSystem::listDirectory("/Home/Settings/Filters/Rules"_s)) {
        // The same list's, of other bytes: its stem and a hash, nothing
        // more - `kosmos-easylist-` is not `kosmos-easylist-cookie-`.
        if (!name.startsWith(stem) || name.length() != stem.length() + 16
            || !name.substring(stem.length()).containsOnly<isASCIIHexDigit>())
            continue;
        auto old = name.substring(strlen("ContentRuleList-"));
        auto id = adoptWK(WKStringCreateWithUTF8CString(old.utf8().data()));
        WKUserContentExtensionStoreRemove(ruleStore(), id.get(), strdup(old.utf8().data()), ruleListRemoved);
    }

    auto id = adoptWK(WKStringCreateWithUTF8CString(pending->identifier));
    WKUserContentExtensionStoreCompile(ruleStore(), id.get(), json.get(), pending, ruleListCompiled);
}

WKUserContentExtensionStoreRef ruleStore()
{
    static WKUserContentExtensionStoreRef store = [] {
        FileSystem::makeAllDirectories("/Home/Settings/Filters/Rules"_s);
        auto path = adoptWK(WKStringCreateWithUTF8CString("/Home/Settings/Filters/Rules"));
        return WKUserContentExtensionStoreCreate(path.get());
    }();
    return store;
}

void startRules()
{
    if (rules.started)
        return;
    rules.started = true;
    rules.from = WTF::MonotonicTime::now();

    struct Folder { const char* path; const char* tag; };
    for (auto folder : { Folder { "/Kosmos/Filters", "kosmos-" }, Folder { "/Home/Settings/Filters", "home-" } }) {
        for (auto& name : FileSystem::listDirectory(String::fromLatin1(folder.path))) {
            size_t suffix = name.endsWith(".json"_s) ? 5 : name.endsWith(".json.gz"_s) ? 8 : 0;
            if (!suffix)
                continue;
            auto path = FileSystem::pathByAppendingComponent(String::fromLatin1(folder.path), name);
            auto bytes = FileSystem::readEntireFile(path);
            if (!bytes)
                continue;
            auto* pending = new Pending;
            pending->path = path;
            snprintf(pending->base, sizeof pending->base, "%s%s", folder.tag, name.left(name.length() - suffix).utf8().data());
            snprintf(pending->identifier, sizeof pending->identifier, "%s-%016llx", pending->base,
                static_cast<unsigned long long>(hashOf(bytes->span())));
            rules.compiling++;
            auto id = adoptWK(WKStringCreateWithUTF8CString(pending->identifier));
            WKUserContentExtensionStoreLookup(ruleStore(), id.get(), pending, ruleListLookedUp);
        }
    }
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

double since(const View* view, WTF::MonotonicTime when)
{
    return (when - view->traceFrom).milliseconds();
}

void urlInto(WKURLRef url, char* into, size_t room)
{
    auto string = url ? adoptWK(WKURLCopyString(url)) : nullptr;
    copy(string.get(), into, room);
}

// The oldest request for this address not yet ended.
Load* loadOf(View* view, WKURLRef url)
{
    char at[sizeof(Load::url)];
    urlInto(url, at, sizeof at);
    for (auto& load : view->loads) {
        if (!strcmp(load.url, at))
            return &load;
    }
    return nullptr;
}

void loadSent(const void* info, WKPageRef, WKURLRequestRef request)
{
    if (!tracing)
        return;
    auto* view = viewOf(info);
    Load load;
    auto url = adoptWK(WKURLRequestCopyURL(request));
    urlInto(url.get(), load.url, sizeof load.url);
    load.sent = WTF::MonotonicTime::now();
    view->loads.append(load);
}

void loadAnswered(const void* info, WKPageRef, WKURLRef url, WKURLResponseRef response)
{
    if (!tracing)
        return;
    if (auto* load = loadOf(viewOf(info), url)) {
        load->answered = WTF::MonotonicTime::now();
        load->answeredYet = true;
        load->status = response ? WKURLResponseHTTPStatusCode(response) : 0;
    }
}

void loadEnded(const void* info, WKPageRef, WKURLRef url, WKURLResponseRef response, WKErrorRef error)
{
    if (!tracing)
        return;
    auto* view = viewOf(info);
    auto* load = loadOf(view, url);
    if (!load)
        return;
    auto now = WTF::MonotonicTime::now();
    int status = load->status ? load->status : (response ? WKURLResponseHTTPStatusCode(response) : 0);
    double first = load->answeredYet ? (load->answered - load->sent).milliseconds() : -1;
    fprintf(stderr, "webkit: load view %d at %6.0f ms, first byte %6.0f, took %6.0f, %3d%s%d %s\n",
           view->id, since(view, load->sent), first, (now - load->sent).milliseconds(), status,
           error ? " error " : "", error ? WKErrorGetErrorCode(error) : 0, load->url);
    if (error)
        view->loadsFailed++;
    else
        view->loadsDone++;
    view->loads.removeFirstMatching([&](const Load& each) { return &each == load; });
}

// What has not ended, oldest first, at most a dozen of them.
void tracePending(View* view, WTF::MonotonicTime now)
{
    unsigned shown = 0;
    for (auto& load : view->loads) {
        if (shown++ == 12)
            break;
        fprintf(stderr, "webkit:   waiting %6.0f ms, %s %s\n", (now - load.sent).milliseconds(),
               load.answeredYet ? "answered" : "no answer", load.url);
    }
}

/*
 * **Where a page's time went, by the page's own clock**: the Navigation,
 * Resource and Paint Timing that WebKit keeps for every page - a request's
 * name lookup, connection, TLS, wait for its first byte and download; the
 * page's own, its DOM ready and its load; its first paint - asked of the
 * page as a script and said in the log, a line a part. Times are ms from
 * the navigation's start. A request from another site that does not allow
 * it (`Timing-Allow-Origin`) has its total and nothing finer: "hidden".
 */
const char* TIMING_SCRIPT = R"JS((function () {
  function ms(x) { return Math.round(x || 0); }
  function d(a, b) { return a > 0 && b > a ? b - a : 0; }      // a phase, never negative
  function phases(e) {
    return "dns " + ms(d(e.domainLookupStart, e.domainLookupEnd))
      + " connect " + ms(d(e.connectStart, e.connectEnd))
      + " tls " + ms(d(e.secureConnectionStart, e.connectEnd))
      + " wait " + ms(d(e.requestStart, e.responseStart))
      + " download " + ms(d(e.responseStart, e.responseEnd));
  }
  var out = [];
  var n = performance.getEntriesByType("navigation")[0];
  if (n) {
    out.push("page " + phases(n) + ", first byte at " + ms(n.responseStart)
      + ", dom ready at " + ms(n.domContentLoadedEventEnd) + ", loaded at " + ms(n.loadEventEnd)
      + ", " + ms(n.transferSize / 1024) + " KB, " + (n.nextHopProtocol || "?"));
  }
  var paints = performance.getEntriesByType("paint").map(function (e) { return e.name + " at " + ms(e.startTime); });
  out.push("paint " + (paints.join(", ") || "none yet"));
  var r = performance.getEntriesByType("resource");
  var sum = { dns: 0, connect: 0, tls: 0, wait: 0, download: 0 }, kinds = {}, bytes = 0, hidden = 0, fresh = 0, last = 0;
  r.forEach(function (e) {
    kinds[e.initiatorType] = (kinds[e.initiatorType] || 0) + 1;
    bytes += e.transferSize || 0;
    last = Math.max(last, e.responseEnd);
    if (!e.requestStart) { hidden++; return; }
    sum.dns += d(e.domainLookupStart, e.domainLookupEnd);
    if (e.connectEnd > e.connectStart) fresh++;
    sum.connect += d(e.connectStart, e.connectEnd);
    sum.tls += d(e.secureConnectionStart, e.connectEnd);
    sum.wait += d(e.requestStart, e.responseStart);
    sum.download += d(e.responseStart, e.responseEnd);
  });
  out.push("requests " + r.length + ", " + ms(bytes / 1024) + " KB, last ended at " + ms(last)
    + ", " + fresh + " on new connections, " + hidden + " hidden; "
    + Object.keys(kinds).map(function (k) { return k + " " + kinds[k]; }).join(" "));
  out.push("summed dns " + ms(sum.dns) + " connect " + ms(sum.connect) + " tls " + ms(sum.tls)
    + " wait " + ms(sum.wait) + " download " + ms(sum.download));
  r.slice().sort(function (a, b) { return b.duration - a.duration; }).slice(0, 8).forEach(function (e) {
    out.push("slow " + ms(e.duration) + " from " + ms(e.startTime) + ", "
      + (e.requestStart ? phases(e) : "hidden") + ", " + e.name.slice(0, 110));
  });
  return out.join("\n");
})())JS";

void timingsSaid(WKTypeRef result, WKErrorRef, void* context)
{
    int id = static_cast<int>(reinterpret_cast<intptr_t>(context));
    if (!result || WKGetTypeID(result) != WKStringGetTypeID())
        return;
    static char text[16384];
    copy(static_cast<WKStringRef>(result), text, sizeof text);
    for (char* line = strtok(text, "\n"); line; line = strtok(nullptr, "\n"))
        fprintf(stderr, "webkit: timing view %d: %s\n", id, line);
}

void askTimings(View* view)
{
    if (!view->options.scripts)
        return;                     // a page that runs nothing is not asked
    static WKStringRef script = WKStringCreateWithUTF8CString(TIMING_SCRIPT);
    WKPageEvaluateJavaScriptInMainFrame(view->page, script,
        reinterpret_cast<void*>(static_cast<intptr_t>(view->id)), timingsSaid);
}

void traceSummary(View* view, const char* how)
{
    if (!tracing)
        return;
    askTimings(view);
    auto now = WTF::MonotonicTime::now();
    fprintf(stderr, "webkit: load view %d %s at %.0f ms: %u requests ended, %u failed, %u still open\n",
           view->id, how, since(view, now), view->loadsDone, view->loadsFailed, static_cast<unsigned>(view->loads.size()));
    tracePending(view, now);
}

WKPageRef createNewPage(WKPageRef, WKPageConfigurationRef, WKNavigationActionRef, WKWindowFeaturesRef, const void*);
void closePage(WKPageRef, const void*);

/*
 * **A view, from a configuration**: the kit's own for one a caller makes,
 * and the one WebKit hands over for one a page opens - `target=_blank`,
 * `window.open` - which carries the page that opened it, and its options.
 */
View* makeView(WKPageConfigurationRef configuration, unsigned width, unsigned height, const struct webkit_options& options)
{
    auto view = std::make_unique<View>();
    view->id = views.nextId++;
    view->options = options;
    view->view = WKViewCreate(configuration);
    if (!view->view)
        return nullptr;
    view->width = width;
    view->height = height;
    WKViewSetSize(view->view, WKSizeMake(width, height));
    WKViewSetVisible(view->view, false);
    WKViewSetActive(view->view, false);

    WKViewClientV0 viewClient { };
    viewClient.base = { 0, view.get() };
    viewClient.setViewNeedsDisplay = needsDisplay;
    WKViewSetViewClient(view->view, &viewClient.base);

    view->page = WKViewGetPage(view->view);
    WKPageSetPageZoomFactor(view->page, zoomFactor);
    WKPageNavigationClientV2 navigation { };
    navigation.base = { 2, view.get() };
    navigation.contentRuleListNotification = ruleListActed;
    navigation.didStartProvisionalNavigation = didStart;
    navigation.didCommitNavigation = didCommit;
    navigation.didFinishNavigation = didFinish;
    navigation.didFailNavigation = didFail;
    navigation.didFailProvisionalNavigation = didFail;
    navigation.webProcessDidCrash = didCrash;
    navigation.decidePolicyForNavigationAction = decidePolicy;
    navigation.didSameDocumentNavigation = didSameDocument;
    navigation.didReceiveAuthenticationChallenge = challenged;
    WKPageSetPageNavigationClient(view->page, &navigation.base);

    WKPageResourceLoadClientV0 loads { };
    loads.base = { 0, view.get() };
    loads.didSendRequest = loadSent;
    loads.didReceiveResponse = loadAnswered;
    loads.didCompleteWithError = loadEnded;
    WKPageSetResourceLoadClient(view->page, &loads.base);

    WKPageUIClientV8 ui { };
    ui.base = { 8, view.get() };
    ui.getWindowFrame = getWindowFrame;
    ui.createNewPage = createNewPage;
    ui.close = closePage;
    WKPageSetPageUIClient(view->page, &ui.base);

    auto* made = view.get();
    views.all.append(WTF::move(view));
    return made;
}

// A link to a new window, or `window.open`: a new view, the size of the one
// that asked and with its options, dark until the caller shows it. A view
// that does not follow links opens none.
WKPageRef createNewPage(WKPageRef, WKPageConfigurationRef configuration, WKNavigationActionRef, WKWindowFeaturesRef, const void* info)
{
    auto* opener = viewOf(info);
    if (!opener->options.follow)
        return nullptr;
    auto* view = makeView(configuration, opener->width, opener->height, opener->options);
    if (!view)
        return nullptr;
    views.opened = view->id;
    views.opener = opener->id;
    views.stop();
    return static_cast<WKPageRef>(const_cast<void*>(WKRetain(view->page)));
}

// `window.close()`: the caller says which view, and closes it.
void closePage(WKPageRef, const void* info)
{
    views.closing = viewOf(info)->id;
    views.stop();
}

// What the pointer, the wheel and keys go to: a view's page, or its
// inspector's while the caller says so (`webkit_target`).
WKPageRef input(View* view)
{
    if (!view)
        return nullptr;
    if (view->toInspector && view->inspector)
        return WKViewGetPage(view->inspector);
    return view->page;
}

WKPageRef pageOf(int id)
{
    auto* view = views.find(id);
    return view ? view->page : nullptr;
}

} // namespace

extern "C" int webkit_view(unsigned width, unsigned height, const struct webkit_options* options)
{
    if (!width || !height)
        return 0;
    struct webkit_options chosen = options ? *options : webkit_options { 1, 1 };

    auto configuration = adoptWK(WKPageConfigurationCreate());
    WKPageConfigurationSetContext(configuration.get(), context());
    WKPageConfigurationSetWebsiteDataStore(configuration.get(), dataStore());
    WKPageConfigurationSetPreferences(configuration.get(), preferences(chosen.scripts));
    WKPageConfigurationSetUserContentController(configuration.get(), contentController());

    // The first view first: making it is what starts WTF's main thread and
    // run loop, which the timer below is made on.
    auto* view = makeView(configuration.get(), width, height, chosen);
    if (!view)
        return 0;
    if (!views.slice) {
        static WTF::RunLoop::Timer slice(WTF::RunLoop::mainSingleton(), "webkit slice"_s, [] {
            views.stop();
        });
        views.slice = &slice;
    }
    startRules();
    return view->id;
}

/*
 * **A view closed**: its inspector and its page closed - which ends its
 * web process, unless another page shares it - and its view let go.
 */
extern "C" void webkit_close(int id)
{
    for (size_t i = 0; i < views.all.size(); i++) {
        auto& view = views.all[i];
        if (view->id != id)
            continue;
        if (view->inspector) {
            WKInspectorClose(WKPageGetInspector(view->page));
            if (view->inspector)
                WKRelease(view->inspector);
            view->inspector = nullptr;
        }
        WKPageClose(view->page);
        WKRelease(view->view);
        views.all.removeAt(i);
        return;
    }
}

extern "C" void webkit_show(int id, int shown)
{
    auto* view = views.find(id);
    if (!view)
        return;
    view->shown = shown != 0;
    WKViewSetVisible(view->view, view->shown);
    WKViewSetActive(view->view, view->shown);
    if (view->inspector) {
        WKViewSetVisible(view->inspector, view->shown);
        WKViewSetActive(view->inspector, view->shown);
    }
    if (view->shown)
        view->happened |= WEBKIT_PICTURE;
    else {
        WKViewSetFocus(view->view, false);
        if (view->inspector)
            WKViewSetFocus(view->inspector, false);
    }
}

extern "C" unsigned webkit_views(struct webkit_view_info* out, unsigned room)
{
    unsigned n = 0;
    for (auto& view : views.all) {
        if (n == room)
            break;
        auto& each = out[n++];
        each.id = view->id;
        each.shown = view->shown;
        each.loading = view->loading || view->asked;
        each.pid = static_cast<int>(view->pid);
        auto title = adoptWK(WKPageCopyTitle(view->page));
        copy(title.get(), each.title, sizeof each.title);
        auto url = adoptWK(WKPageCopyActiveURL(view->page));
        auto string = url ? adoptWK(WKURLCopyString(url.get())) : nullptr;
        copy(string.get(), each.address, sizeof each.address);
        // A page that stopped has no address left in WebKit: the one kept.
        if (!each.address[0] && view->crashed)
            snprintf(each.address, sizeof each.address, "%s", view->crashedAddress);
        if (!each.title[0])
            snprintf(each.title, sizeof each.title, "%s", each.address);
    }
    return n;
}

extern "C" void webkit_frame(int x, int y, unsigned width, unsigned height)
{
    windowFrame = WKRectMake(x, y, width, height);
}

extern "C" void webkit_resize(int id, unsigned width, unsigned height)
{
    auto* view = views.find(id);
    if (!view || !width || !height)
        return;
    view->width = width;
    view->height = height;
    WKViewSetSize(view->view, WKSizeMake(width, height));
}

namespace {

void releaseHeld()
{
    for (auto& each : views.all) {
        if (!each->held[0])
            continue;
        auto url = adoptWK(WKURLCreateWithUTF8CString(each->held));
        each->held[0] = '\0';
        WKPageLoadURL(each->page, url.get());
    }
}

} // namespace

extern "C" void webkit_load(int id, const char* address)
{
    auto page = pageOf(id);
    if (!page || !address)
        return;
    // A page asked for is no longer the one refused: said at once, not
    // when WebKit begins it, so the caller's own page gives way now.
    if (auto* view = views.find(id))
        view->refused = false;
    if (rules.compiling) {
        auto* view = views.find(id);
        snprintf(view->held, sizeof view->held, "%s", address);
        return;
    }
    auto url = adoptWK(WKURLCreateWithUTF8CString(address));
    WKPageLoadURL(page, url.get());
}

extern "C" void webkit_back(int id)
{
    auto page = pageOf(id);
    if (page && WKPageCanGoBack(page))
        WKPageGoBack(page);
}

extern "C" void webkit_forward(int id)
{
    auto page = pageOf(id);
    if (page && WKPageCanGoForward(page))
        WKPageGoForward(page);
}

extern "C" void webkit_reload(int id)
{
    if (auto page = pageOf(id))
        WKPageReload(page);
}

extern "C" void webkit_stop(int id)
{
    if (auto* view = views.find(id)) {
        WKPageStopLoading(view->page);
        view->asked = false; // stopped before it began says nothing
    }
}

/*
 * **Woken when WebKit has news** (`testing.md` 18.855). WebKit's messages
 * are handled only inside a slice (`webkit_pump`), and a slice runs after
 * the window's poll - so a picture that arrived between two waited for the
 * poll to time out. Given a code and a door to the window manager
 * (`webkit_wake`), a connection's news pulses it once a slice; the manager
 * answers the window's poll with a `changed` event and the loop pumps now.
 * Called on WebKit's receiving threads, so it touches only atomics.
 */
std::atomic<long> wakeDoor { -1 };
std::atomic<uint32_t> wakeCode { 0 };
std::atomic<bool> woken { false };

void messagesArrived()
{
    long door = wakeDoor.load(std::memory_order_relaxed);
    if (door < 0 || woken.exchange(true))
        return;
    (void)kosmos_pulse(door, wakeCode.load(std::memory_order_relaxed), 0);
}

extern "C" void webkit_wake(long door, unsigned code)
{
    wakeCode.store(code, std::memory_order_relaxed);
    wakeDoor.store(door, std::memory_order_relaxed);
    WKKosmosOnMessages(door >= 0 ? messagesArrived : nullptr);
}

extern "C" void webkit_trace(int on)
{
    tracing = on;
    WKKosmosSetFramesTraced(on);    // and a page's process, when it starts
}

extern "C" void webkit_pump(unsigned ms, struct webkit_events* out)
{
    // News from here on wakes the window again: what arrived before is
    // handled in this slice.
    woken.store(false);
    memset(out, 0, sizeof *out);
    collect();
    bool waiting = false;
    for (auto& each : views.all) {
        if (auto pid = WKPageGetProcessIdentifier(each->page))
            each->pid = pid;
        if (each->shown && each->happened)
            waiting = true;
    }
    if (views.all.isEmpty())
        return;

    // Something already waiting to be said is said now, without a slice.
    if (tracing) {
        bool slicing = !waiting && !views.opened && !views.closing && ms;
        auto now = WTF::MonotonicTime::now();
        for (auto& each : views.all) {
            if (!each->wheelWaiting || each->wheelSliced)
                continue;
            if (!slicing) {
                each->unsliced++;
                continue;
            }
            each->toSlice.add((now - each->wheelAt).milliseconds());
            each->wheelSliced = true;
        }
    }
    if (!waiting && !views.opened && !views.closing && ms) {
        views.running = true;
        views.slice->startOneShot(WTF::Seconds::fromMilliseconds(ms));
        WTF::RunLoop::run();
        views.running = false;
        views.slice->stop();
    }

    // A traced page still loading: what it waits on, every five seconds;
    // and its frames, once they have stopped for a second.
    if (tracing) {
        auto now = WTF::MonotonicTime::now();
        for (auto& each : views.all)
            framesSaid(each.get(), now);
        for (auto& each : views.all) {
            if (each->loading && now - each->lastReport >= 5_s) {
                each->lastReport = now;
                traceSummary(each.get(), "still loading");
            }
        }
    }

    // A view in the dark that began, finished or ended: its caller draws
    // it again, for its title and whether it is loading.
    for (auto& each : views.all) {
        if (!each->shown && each->happened & ~WEBKIT_PICTURE) {
            out->others = 1;
            each->happened &= WEBKIT_PICTURE;
        }
    }
    out->opened = views.opened;
    out->opener = views.opener;
    out->closing = views.closing;
    views.opened = views.opener = views.closing = 0;
}

extern "C" int webkit_state(int id, struct webkit_state* out)
{
    memset(out, 0, sizeof *out);
    auto* view = views.find(id);
    if (!view)
        return 0;

    out->happened = view->happened;
    out->error = view->error;
    view->happened = 0;
    view->error = 0;

    // Asked and never begun - a navigation a script cancelled, say - is
    // not loading for ever: thirty seconds is longer than any start.
    if (view->asked && WTF::MonotonicTime::now() - view->askedAt > 30_s)
        view->asked = false;
    out->loading = view->loading || view->asked;
    if (view->asked || out->happened & WEBKIT_ASKED)
        memcpy(out->going, view->going, sizeof out->going);
    out->progress = WKPageGetEstimatedProgress(view->page);
    out->can_back = WKPageCanGoBack(view->page);
    out->can_forward = WKPageCanGoForward(view->page);
    out->blocked = view->blocked;
    out->cookies_blocked = view->cookiesBlocked;
    out->unblocked = view->unblocked;
    if (view->refused) {
        out->refused = 1;
        out->certificate_error = view->refusedError;
        memcpy(out->refused_reason, view->refusedReason, sizeof out->refused_reason);
        memcpy(out->refused_address, view->refusedAddress, sizeof out->refused_address);
    }
    if (view->held[0])
        out->loading = 1;

    auto title = adoptWK(WKPageCopyTitle(view->page));
    copy(title.get(), out->title, sizeof out->title);

    auto url = adoptWK(WKPageCopyActiveURL(view->page));
    if (url) {
        auto string = adoptWK(WKURLCopyString(url.get()));
        copy(string.get(), out->address, sizeof out->address);
    }
    return 1;
}

extern "C" int webkit_security(int id, struct webkit_security* out)
{
    memset(out, 0, sizeof *out);
    auto* view = views.find(id);
    if (!view)
        return 0;

    auto url = adoptWK(WKPageCopyCommittedURL(view->page));
    auto scheme = url ? adoptWK(WKURLCopyScheme(url.get())) : nullptr;
    char how[16] = "";
    if (scheme)
        copy(scheme.get(), how, sizeof how);

    WKSecurityKosmos seen;
    WKPageGetSecurityKosmos(view->page, &seen);
    out->error = seen.verificationError;
    out->chain = seen.chainLength;
    memcpy(out->subject, seen.subject, sizeof out->subject);
    memcpy(out->issuer, seen.issuer, sizeof out->issuer);
    memcpy(out->names, seen.names, sizeof out->names);
    out->valid_from = seen.validFrom;
    out->valid_until = seen.validUntil;

    if (!strcmp(how, "file"))
        out->came = WEBKIT_CAME_FILE;
    else if (!strcmp(how, "http"))
        out->came = WEBKIT_CAME_PLAIN;
    else if (!strcmp(how, "https")) {
        char host[256];
        serverOf(url.get(), host, sizeof host);
        if (view->anyway[0] && !strcmp(host, view->anyway))
            out->came = WEBKIT_CAME_ANYWAY;
        else if (seen.insecureContent || !seen.secureOnly)
            out->came = WEBKIT_CAME_MIXED;
        else
            out->came = WEBKIT_CAME_SECURE;
    } else
        out->came = WEBKIT_CAME_NONE;
    return 1;
}

extern "C" unsigned webkit_kept(int id, struct webkit_kept* out, unsigned room)
{
    auto* view = views.find(id);
    if (!view)
        return 0;
    auto list = view->kept;
    std::sort(list.begin(), list.end(), [](auto& a, auto& b) { return a.count > b.count; });
    unsigned n = std::min<unsigned>(room, list.size());
    for (unsigned i = 0; i < n; i++)
        out[i] = list[i];
    return list.size();
}

extern "C" void webkit_allow_sites(const char* list)
{
    allowedSites.clear();
    for (const char* at = list ? list : ""; *at;) {
        size_t length = strcspn(at, "\n");
        const char* from = at;
        const char* to = at + length;
        while (from < to && (*from == ' ' || *from == '\t' || *from == '\r'))
            from++;
        while (to > from && (to[-1] == ' ' || to[-1] == '\t' || to[-1] == '\r'))
            to--;
        if (to > from)
            allowedSites.append(String::fromUTF8(std::span<const char>(from, to - from)).convertToASCIILowercase());
        at += length;
        if (*at)
            at++;
    }
    fprintf(stderr, "webkit: %zu site%s allowed past blocking\n",
        static_cast<size_t>(allowedSites.size()), allowedSites.size() == 1 ? "" : "s");
}

extern "C" int webkit_anyway(int id)
{
    auto* view = views.find(id);
    if (!view || !view->refused || !view->refusedHost[0])
        return 0;
    memcpy(view->anyway, view->refusedHost, sizeof view->anyway);
    view->refused = false;
    fprintf(stderr, "webkit: view %d opens %s anyway, for this tab\n", view->id, view->anyway);
    webkit_load(id, view->refusedAddress);
    return 1;
}

extern "C" int webkit_crash(int id, struct webkit_crash* out)
{
    memset(out, 0, sizeof *out);
    auto* view = views.find(id);
    if (!view || !view->crashed)
        return 0;
    collect();
    out->pid = static_cast<int>(view->crashedPid);
    memcpy(out->address, view->crashedAddress, sizeof out->address);
    if (auto* d = deathOf(view->crashedPid)) {
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

extern "C" void webkit_paint(int id, uint32_t* pixels, unsigned width, unsigned height, unsigned pitch)
{
    auto page = pageOf(id);
    if (!page)
        return;
    auto* view = tracing ? views.find(id) : nullptr;
    auto from = view ? WTF::MonotonicTime::now() : WTF::MonotonicTime();
    WKPagePaintKosmos(page, pixels, width, height, pitch, WKRectMake(0, 0, width, height));
    if (view) {
        auto now = WTF::MonotonicTime::now();
        frameSeen(view, now);
        view->drawn.add((now - from).milliseconds());
    }
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
    return button == WEBKIT_LEFT ? 1 : button == WEBKIT_RIGHT ? 2 : button == WEBKIT_MIDDLE ? 4 : 0;
}

}

extern "C" void webkit_mouse(int id, enum webkit_mouse action, enum webkit_button button, int x, int y, unsigned mods)
{
    auto page = input(views.find(id));
    if (!page)
        return;

    if (action == WEBKIT_PRESS) {
        auto now = MonotonicTime::now();
        bool again = button == pointer.button && now - pointer.at < doubleClickTime
            && abs(x - pointer.x) <= doubleClickRoom && abs(y - pointer.y) <= doubleClickRoom;
        pointer.clicks = again ? pointer.clicks + 1 : 1;
        pointer.button = button;
        pointer.x = x;
        pointer.y = y;
        pointer.at = now;
        pointer.held |= heldBit(button);
        WKPageMouseKosmos(page, kWKKosmosMouseDown, button, pointer.held, x, y, pointer.clicks, modifiersOf(mods));
    } else if (action == WEBKIT_RELEASE) {
        pointer.held &= ~heldBit(button);
        WKPageMouseKosmos(page, kWKKosmosMouseUp, button, pointer.held, x, y, pointer.clicks, modifiersOf(mods));
    } else {
        // A move with a button held is a drag of that button, as WebKit has it.
        int moving = pointer.held & 1 ? WEBKIT_LEFT : pointer.held & 4 ? WEBKIT_MIDDLE : pointer.held & 2 ? WEBKIT_RIGHT : WEBKIT_NONE;
        WKPageMouseKosmos(page, kWKKosmosMouseMove, moving, pointer.held, x, y, 0, modifiersOf(mods));
    }
}

//
// **The wheel**: a notch is three of WebKit's lines, 40 pixels each, as
// the kit scrolls three rows a notch (`ui.WHEEL_ROWS`). Shift turns it
// sideways, as it does on every desktop.
//
extern "C" void webkit_wheel(int id, int x, int y, int notches, unsigned mods)
{
    auto* view = views.find(id);
    auto page = input(view);
    if (!page || !notches)
        return;
    if (tracing && !view->wheelWaiting) {
        view->wheelAt = WTF::MonotonicTime::now();
        view->wheelWaiting = true;
        view->wheelSliced = false;
    }
    float ticks = notches, pixels = notches * 3 * 40;
    if (mods & 1)
        WKPageWheelKosmos(page, x, y, pixels, 0, ticks, 0, modifiersOf(mods & ~1u));
    else
        WKPageWheelKosmos(page, x, y, 0, pixels, 0, ticks, modifiersOf(mods));
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

void send(WKPageRef page, const char* key, const char* dom, const char* text, int virtualKey, unsigned mods)
{
    WKPageKeyKosmos(page, true, key, dom, text, virtualKey, modifiersOf(mods));
    WKPageKeyKosmos(page, false, key, dom, nullptr, virtualKey, modifiersOf(mods));
}

}

extern "C" int webkit_key(int id, int c)
{
    auto page = input(views.find(id));
    if (!page)
        return 0;

    // `keys.parts`: the key, and its modifiers in steps of 1024 below it.
    unsigned mods = (255 - c) / 1024;
    int key = c + 1024 * (int)mods;

    for (auto& named : namedKeys) {
        if (named.code == key) {
            send(page, named.key, named.dom, nullptr, named.virtualKey, mods);
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
        send(page, name, name, nullptr, 0x6f + n, mods);
        return 1;
    }
    if (key < 0 || key > 255)
        return 0;

    switch (key) {
    case 8: case 127:
        send(page, "Backspace", "Backspace", nullptr, 0x08, mods);
        return 1;
    case 9:
        send(page, "Tab", "Tab", "\t", 0x09, mods);
        return 1;
    case 10: case 13:
        send(page, "Enter", "Enter", "\r", 0x0d, mods);
        return 1;
    case 27:
        send(page, "Escape", "Escape", nullptr, 0x1b, mods);
        return 1;
    }

    // Control with a letter arrives as the letter's control character.
    if (key >= 1 && key <= 26) {
        char letter[2] = { (char)('a' + key - 1), 0 };
        char dom[5] = { 'K', 'e', 'y', (char)('A' + key - 1), 0 };
        send(page, letter, dom, nullptr, 'A' + key - 1, mods | 4);
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
    send(page, text, dom[0] ? dom : "Unidentified", text, virtualKey, mods);
    return 1;
}

extern "C" void webkit_focus(int id, int focused)
{
    auto* view = views.find(id);
    if (!view)
        return;
    bool toInspector = view->toInspector && view->inspector;
    WKViewSetFocus(view->view, focused && !toInspector);
    if (view->inspector)
        WKViewSetFocus(view->inspector, focused && toInspector);
}

extern "C" void webkit_developer(int on)
{
    WKPreferencesSetDeveloperExtrasEnabled(preferences(true), on);
}

/*
 * **What JetSki's Settings say of every page** (`docs/webkit.html`,
 * *Pages*; `roadmap.md`, S1): scripts on or off for the views that run
 * them - a view made without scripts keeps them off whatever this says -
 * and pictures loaded or not for all of them. One object a kind, so a
 * change reaches every open tab at once, as Settings promises.
 */
extern "C" void webkit_settings(int scripts, int images)
{
    WKPreferencesSetJavaScriptEnabled(preferences(true), scripts);
    WKPreferencesSetLoadsImagesAutomatically(preferences(true), images);
    WKPreferencesSetLoadsImagesAutomatically(preferences(false), images);
}

/*
 * **The zoom, all of a page** (Settings' *Zoom*, Chrome's four): WebKit's
 * page zoom, which scales words, pictures and boxes together, for every
 * view there is and every one made after.
 */
extern "C" void webkit_zoom(int percent)
{
    zoomFactor = percent / 100.0;
    for (auto& view : views.all)
        WKPageSetPageZoomFactor(view->page, zoomFactor);
}

extern "C" void webkit_inspector_files(const char* folder)
{
    WKInspectorSetResourcesKosmos(folder);
    WKInspectorSetViewHandlerKosmos(inspectorView, nullptr);
}

extern "C" void webkit_inspect(int id, int show)
{
    auto* view = views.find(id);
    if (!view || !view->options.scripts)
        return;
    auto inspector = WKPageGetInspector(view->page);
    if (show) {
        WKInspectorShow(inspector);
        WKInspectorAttach(inspector);
    } else
        WKInspectorClose(inspector);
}

extern "C" int webkit_inspecting(int id)
{
    auto* view = views.find(id);
    return view && view->inspector;
}

extern "C" void webkit_inspector_size(unsigned width, unsigned height)
{
    if (!width || !height)
        return;
    views.inspectorWidth = width;
    views.inspectorHeight = height;
    for (auto& view : views.all) {
        if (view->inspector)
            WKViewSetSize(view->inspector, WKSizeMake(width, height));
    }
}

extern "C" void webkit_paint_inspector(int id, uint32_t* pixels, unsigned width, unsigned height, unsigned pitch)
{
    auto* view = views.find(id);
    if (!view || !view->inspector)
        return;
    WKPagePaintKosmos(WKViewGetPage(view->inspector), pixels, width, height, pitch, WKRectMake(0, 0, width, height));
}

extern "C" void webkit_target(int id, int inspector)
{
    if (auto* view = views.find(id))
        view->toInspector = inspector != 0;
}
