/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **W6b's WebCore test** (`docs/w6.md`; `tools/run_webcore.py`): WebCore
 * built as Kosmos's port, making pages with no window - what WebKit's
 * `testing/TestPage.cpp` does for its own unit tests - and saying each
 * one's render tree, the text WebKit's LayoutTests compare
 * (`externalRepresentation`).
 *
 * What it says is a line `page NAME`, then the tree a line at a time, each
 * after `tree ` so that nothing else on the serial line can be taken for
 * one, then `webcoretest: N of M pages`.
 *
 * Each page is a file in `FOLDER/pages/`, given to a Page whose clients
 * are WebKit's empty ones, in a frame 800 by 600, with the fonts in
 * `FOLDER/fonts/` (`FontFolderKosmos.h`). There is no GPU: the process
 * says so before the first page (`ProcessCapabilities`), and WebKit
 * paints on the processor - the EGL Kit, which has no display, is never
 * asked for a context.
 *
 * Scripts run: JavaScriptCore is the one W4 built, and a page's script
 * changing its own tree is part of what a render tree says.
 */

#include "webcoretest.h"

#include <JavaScriptCore/InitializeThreading.h>
#include <WebCore/Document.h>
/* After a header that defines WEBCORE_EXPORT, which it uses and does not. */
#include <WebCore/BlobRegistry.h>
#include <WebCore/CachedResourceLoader.h>
#include <WebCore/DeprecatedGlobalSettings.h>
#include <WebCore/DocumentLoader.h>
#include <WebCore/DocumentView.h>
#include <WebCore/DocumentWriter.h>
#include <WebCore/EmptyClients.h>
#include <WebCore/FontFolderKosmos.h>
#include <WebCore/FontRenderOptions.h>
#include <WebCore/FrameLoader.h>
#include <WebCore/GraphicsContextSkia.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/LocalDOMWindow.h>
#include <WebCore/LocalFrameView.h>
#include <WebCore/LoaderStrategy.h>
#include <WebCore/MIMETypeRegistry.h>
#include <WebCore/NetworkLoadMetrics.h>
#include <WebCore/ResourceResponse.h>
#include <WebCore/SharedBuffer.h>
#include <WebCore/SubresourceLoader.h>
#include <WebCore/MediaStrategy.h>
#include <WebCore/Page.h>
#include <WebCore/PageConfiguration.h>
#include <WebCore/PlatformStrategies.h>
#include <WebCore/ProcessCapabilities.h>
#include <WebCore/RenderTreeAsText.h>
#include <WebCore/ResourceError.h>
#include <WebCore/Settings.h>
#include <pal/SessionID.h>
#include <wtf/FileSystem.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringView.h>

#include <stdio.h>

#include "include/core/SkSurface.h"

using namespace WebCore;

static webcoretest_say_fn g_say;
static void* g_ctx;

static void say(const String& line)
{
    g_say(line.utf8().data(), g_ctx);
}

// One page, from its file, laid out at `size`: the Page, or null with the
// reason said. Its main frame holds the document and the view.
static RefPtr<Page> pageFrom(const String& folder, const String& name, IntSize size)
{
    auto bytes = FileSystem::readEntireFile(makeString(folder, "/pages/"_s, name));
    if (!bytes) {
        say(makeString("webcoretest: no page "_s, name));
        return nullptr;
    }

    // Empty clients make the frame an SVG picture's: sandboxed in every way,
    // scripts included. A page's frame is sandboxed in none.
    auto configuration = pageConfigurationWithEmptyClients(std::nullopt, PAL::SessionID::defaultSessionID());
    if (auto* local = std::get_if<PageConfiguration::LocalMainFrameCreationParameters>(&configuration.mainFrameCreationParameters))
        local->effectiveSandboxFlags = { };
    Ref page = Page::create(WTF::move(configuration));
    page->settings().setAcceleratedCompositingEnabled(false);
    page->settings().setScriptEnabled(true);
    // And its pictures fetched as they are met, as WebKitTestRunner's are.
    page->settings().setLoadsImagesAutomatically(true);

    RefPtr frame = page->localMainFrame();
    if (!frame) {
        say("webcoretest: a page with no frame"_s);
        return nullptr;
    }
    frame->setView(LocalFrameView::create(*frame, size));
    frame->init();

    // The page's bytes as a loaded page's go in: the parser decodes them
    // (a byte-order mark is not text, a page may name its encoding), runs
    // its scripts as it reaches them, and may yield to the run loop.
    auto url = URL::fileURLWithFileSystemPath(makeString(folder, "/pages/"_s, name));
    RefPtr loader = frame->loader().activeDocumentLoader();
    auto& writer = loader->writer();
    writer.setMIMEType("text/html"_s);
    writer.begin(url);
    writer.addData(SharedBuffer::create(WTF::move(*bytes)));
    writer.end();

    RefPtr document = frame->document();
    if (!document) {
        say(makeString("webcoretest: no document for "_s, name));
        return nullptr;
    }

    // Then the run loop, until the load event has been and gone - what
    // the page's files, scripts and timers did by then is the page, as
    // WebKitTestRunner dumps it then.
    auto deadline = MonotonicTime::now() + 20_s;
    while (!document->loadEventFinished() && MonotonicTime::now() < deadline)
        RunLoop::cycle();
    if (!document->loadEventFinished())
        say(makeString("webcoretest: "_s, name, " did not finish loading"_s));

    document->updateLayoutIgnorePendingStylesheets();
    return page;
}

// One page laid out at 800 by 600; its render tree said a line at a time.
static bool treeOf(const String& folder, const String& name)
{
    RefPtr page = pageFrom(folder, name, IntSize { 800, 600 });
    if (!page)
        return false;

    say(makeString("page "_s, name));
    RefPtr frame = page->localMainFrame();
    auto tree = externalRepresentation(frame.get());
    for (auto line : StringView(tree).split('\n'))
        say(makeString("tree "_s, line));
    // And where the page was scrolled to, as WebKitTestRunner adds after
    // the tree (`dumpFrameScrollPosition`): its window's offsets, when
    // either is not nought.
    if (RefPtr window = frame->document()->window()) {
        double x = window->scrollX(), y = window->scrollY();
        if (std::abs(x) > 0.00000001 || std::abs(y) > 0.00000001)
            say(makeString("tree scrolled to "_s, x, ',', y));
    }
    return true;
}

/*
 * **The strategies a page asks the program that made it for** - how to
 * load, play media and hold blobs. WebKit's web process and WebKitLegacy
 * each bring theirs, and neither is in this source. Here a page's bytes
 * are written into its document, and its files - the pictures, style
 * sheets and scripts beside it - come from `/Home` through `FileLoader`
 * (W6d, `docs/w6.md`), answered on the main run loop as a network
 * process's answers arrive in WebKit's web process; anything but a file is
 * refused. A browser's loader is the network process's, at W7. Media plays
 * nothing and blobs are not kept.
 */
namespace {

class FileLoader final : public LoaderStrategy {
public:
    void loadResource(LocalFrame& frame, CachedResource& resource, ResourceRequest&& request,
        const ResourceLoaderOptions& options, CompletionHandler<void(RefPtr<SubresourceLoader>&&)>&& done) final
    {
        SubresourceLoader::create(frame, resource, WTF::move(request), options,
            [this, done = WTF::move(done)](RefPtr<SubresourceLoader>&& loader) mutable {
                if (loader) {
                    RunLoop::mainSingleton().dispatch([this, loader = Ref { *loader }] {
                        serve(loader);
                    });
                }
                done(WTF::move(loader));
            });
    }

    void loadResourceSynchronously(FrameLoader&, ResourceLoaderIdentifier, const ResourceRequest& request,
        ClientCredentialPolicy, const FetchOptions&, const HTTPHeaderMap&, ResourceError& error,
        ResourceResponse& response, Vector<uint8_t>& data) final
    {
        auto bytes = read(request.url());
        if (!bytes) {
            error = fileDoesNotExistError(ResourceResponse(URL { request.url() }, { }, 0, { }));
            return;
        }
        response = responseFor(request.url(), bytes->size());
        data = WTF::move(*bytes);
    }
    void pageLoadCompleted(Page&) final { }
    void browsingContextRemoved(LocalFrame&) final { }
    void remove(ResourceLoader*) final { }
    void setDefersLoading(ResourceLoader&, bool) final { }
    void crossOriginRedirectReceived(ResourceLoader*, const URL&) final { }
    void servePendingRequests(ResourceLoadPriority) final { }
    void suspendPendingRequests() final { }
    void resumePendingRequests() final { }
    void startPingLoad(LocalFrame&, ResourceRequest& request, const HTTPHeaderMap&, const FetchOptions&,
        ContentSecurityPolicyImposition, PingLoadCompletionHandler&& done) final
    {
        if (done)
            done(cancelledError(request), { });
    }
    void preconnectTo(FrameLoader&, ResourceRequest&& request, StoredCredentialsPolicy, ShouldPreconnectAsFirstParty,
        PreconnectCompletionHandler&& done) final
    {
        if (done)
            done(cancelledError(request));
    }
    void setCaptureExtraNetworkLoadMetricsEnabled(bool) final { }
    bool isOnLine() const final { return false; }
    void addOnlineStateChangeListener(Function<void(bool)>&&) final { }
    void isResourceLoadFinished(CachedResource&, CompletionHandler<void(bool)>&& done) final { done(true); }

    ResourceError cancelledError(const ResourceRequest& request) const final { return refused(request.url(), "cancelled"_s); }
    ResourceError blockedError(const ResourceRequest& request) const final { return refused(request.url(), "blocked"_s); }
    bool isBlockedError(const ResourceError&) const final { return false; }
    ResourceError blockedByContentBlockerError(const ResourceRequest& request) const final { return refused(request.url(), "blocked"_s); }
    ResourceError cannotShowURLError(const ResourceRequest& request) const final { return refused(request.url(), "cannot show"_s); }
    ResourceError interruptedForPolicyChangeError(const ResourceRequest& request) const final { return refused(request.url(), "interrupted"_s); }
    ResourceError cannotShowMIMETypeError(const ResourceResponse& response) const final { return refused(response.url(), "cannot show"_s); }
    ResourceError fileDoesNotExistError(const ResourceResponse& response) const final { return refused(response.url(), "no such file"_s); }
    ResourceError httpsUpgradeRedirectLoopError(const ResourceRequest& request) const final { return refused(request.url(), "redirect loop"_s); }
    ResourceError httpNavigationWithHTTPSOnlyError(const ResourceRequest& request) const final { return refused(request.url(), "HTTPS only"_s); }
    bool isHttpNavigationWithHTTPSOnlyError(const ResourceError&) const final { return false; }
    ResourceError pluginWillHandleLoadError(const ResourceResponse& response) const final { return refused(response.url(), "plug-in"_s); }

private:
    // A file URL's bytes, or nothing for one that is not a file or not there.
    static std::optional<Vector<uint8_t>> read(const URL& url)
    {
        if (!url.protocolIsFile())
            return std::nullopt;
        return FileSystem::readEntireFile(url.fileSystemPath());
    }

    static ResourceResponse responseFor(const URL& url, size_t length)
    {
        return ResourceResponse(URL { url }, MIMETypeRegistry::mimeTypeForPath(url.path()),
            static_cast<long long>(length), { });
    }

    // The answer, all of it at once: the response, the bytes, the end.
    void serve(SubresourceLoader& loader)
    {
        if (loader.reachedTerminalState())
            return;
        URL url = loader.url();
        auto bytes = read(url);
        if (!bytes) {
            loader.didFail(url.protocolIsFile()
                ? fileDoesNotExistError(ResourceResponse(URL { url }, { }, 0, { }))
                : blockedError(loader.request()));
            return;
        }
        size_t length = bytes->size();
        Ref buffer = SharedBuffer::create(WTF::move(*bytes));
        loader.didReceiveResponse(responseFor(url, length),
            [loader = Ref { loader }, buffer = WTF::move(buffer), length] {
                if (loader->reachedTerminalState())
                    return;
                loader->didReceiveBuffer(buffer, static_cast<long long>(length), DataPayloadWholeResource);
                if (!loader->reachedTerminalState())
                    loader->didFinishLoading(NetworkLoadMetrics { });
            });
    }

    static ResourceError refused(const URL& url, ASCIILiteral why)
    {
        return ResourceError(errorDomainWebKitInternal, 0, url, String(why), ResourceError::Type::Cancellation);
    }
};

class NoMedia final : public MediaStrategy {
};

class NoBlobs final : public BlobRegistry {
    void registerInternalFileBlobURL(const URL&, Ref<BlobDataFileReference>&&, const String&, const String&) final { }
    void registerInternalBlobURL(const URL&, Vector<BlobPart>&&, const String&) final { }
    void registerBlobURL(const URL&, const URL&, const PolicyContainer&, const std::optional<SecurityOriginData>&) final { }
    void registerInternalBlobURLOptionallyFileBacked(const URL&, const URL&, RefPtr<BlobDataFileReference>&&, const String&) final { }
    void registerInternalBlobURLForSlice(const URL&, const URL&, long long, long long, const String&) final { }
    void unregisterBlobURL(const URL&, const std::optional<SecurityOriginData>&) final { }
    void registerBlobURLHandle(const URL&, const std::optional<SecurityOriginData>&) final { }
    void unregisterBlobURLHandle(const URL&, const std::optional<SecurityOriginData>&) final { }
    String blobType(const URL&) final { return emptyString(); }
    unsigned long long blobSize(const URL&) final { return 0; }
    void writeBlobsToTemporaryFilesForIndexedDB(const Vector<String>&, CompletionHandler<void(Vector<String>&&)>&& done) final { done({ }); }
};

class Strategies final : public PlatformStrategies {
    LoaderStrategy* createLoaderStrategy() final { return new FileLoader; }
    PasteboardStrategy* createPasteboardStrategy() final { return nullptr; }
    MediaStrategy* createMediaStrategy() final { return new NoMedia; }
    BlobRegistry* createBlobRegistry() final { return new NoBlobs; }
};

} // namespace

// What every call does first: WebKit's threads and JavaScriptCore once,
// and the fonts from this folder.
static void start(const char* folder, webcoretest_say_fn fn, void* ctx)
{
    g_say = fn;
    g_ctx = ctx;

    static bool started = [] {
        WTF::initializeMainThread();
        JSC::initialize();
        static NeverDestroyed<Strategies> strategies;
        setPlatformStrategies(&strategies.get());
        ProcessCapabilities::setCanUseAcceleratedBuffers(false);
        // As WebKitTestRunner's web process does for GTK's and WPE's
        // tests: no hinting and no subpixel positioning, so every advance
        // is a whole pixel and a tree reads as their expected text.
        FontRenderOptions::singleton().disableHintingForTesting();
        // And scroll bars of a fixed fifteen pixels, the same on every
        // port, as run-webkit-tests asks for: a page wider than the view
        // keeps them out of its height.
        DeprecatedGlobalSettings::setMockScrollbarsEnabled(true);
        return true;
    }();
    (void)started;

    auto fonts = makeString(String::fromUTF8(folder), "/fonts/"_s);
    setKosmosFontFolder(fonts.utf8().data());
}

extern "C" int webcoretest_run(const char* folder, const char* const* pages, int count,
                               webcoretest_say_fn fn, void* ctx)
{
    start(folder, fn, ctx);

    int made = 0;
    for (int i = 0; i < count; i++) {
        if (treeOf(String::fromUTF8(folder), String::fromUTF8(pages[i])))
            made++;
    }
    say(makeString("webcoretest: "_s, made, " of "_s, count, " pages"_s));
    return made == count ? made : -1;
}

/*
 * **The page painted** (`docs/w6.md` W6c): laid out at the picture's size
 * and painted by WebCore, through Skia, into the pixels it is given - a
 * Kosmos surface's, 0xAARRGGBB in a little-endian word, which is Skia's
 * BGRA. WebCore draws as it does for a window with no GPU: every layer on
 * the processor, into one canvas.
 */
extern "C" int webcoretest_paint(const char* folder, const char* name, uint32_t* pixels,
                                 unsigned width, unsigned height, unsigned pitch,
                                 webcoretest_say_fn fn, void* ctx)
{
    start(folder, fn, ctx);

    IntSize size { static_cast<int>(width), static_cast<int>(height) };
    RefPtr page = pageFrom(String::fromUTF8(folder), String::fromUTF8(name), size);
    if (!page)
        return -1;

    auto info = SkImageInfo::Make(static_cast<int>(width), static_cast<int>(height),
        kBGRA_8888_SkColorType, kPremul_SkAlphaType);
    auto surface = SkSurfaces::WrapPixels(info, pixels, pitch);
    if (!surface) {
        say("webcoretest: Skia would not draw into the surface"_s);
        return -1;
    }

    RefPtr frame = page->localMainFrame();
    RefPtr view = frame->view();
    view->updateLayoutAndStyleIfNeededRecursive();

    // What layout asked for after the load event - a web font, a picture
    // below the first screen - is let arrive before the snapshot, or text
    // in a font still loading is painted invisible, as a browser draws it
    // for its first moment and never after. Ten seconds at most.
    RefPtr loader = frame->loader().activeDocumentLoader();
    // A font file is a request like any other while it loads. A page that
    // asked for nothing is painted at once; one that did is painted when
    // the count is back at nought, after one more pass of the run loop for
    // what an arrival set going - a face swapped in, a layout.
    auto deadline = MonotonicTime::now() + 10_s;
    bool waited = false;
    for (;;) {
        view->updateLayoutAndStyleIfNeededRecursive();
        int pending = loader->cachedResourceLoader().requestCount();
        if (!pending && waited) {
            RunLoop::cycle();
            view->updateLayoutAndStyleIfNeededRecursive();
        }
        if (!pending)
            break;
        if (MonotonicTime::now() >= deadline) {
            say(makeString("webcoretest: "_s, String::fromUTF8(name), " still loading "_s,
                pending, " files when painted"_s));
            break;
        }
        waited = true;
        RunLoop::cycle();
    }
    // A picture of the page is a snapshot: every image in it decoded
    // before it is drawn, as WebKit's own snapshots do, rather than left
    // for a decoding thread a later frame would pick up.
    view->setPaintBehavior(view->paintBehavior() | PaintBehavior::ForceSynchronousImageDecode);
    {
        GraphicsContextSkia context(*surface->getCanvas(), RenderingMode::Unaccelerated, RenderingPurpose::DOM);
        view->paint(context, IntRect { { }, size });
    }
    say(makeString("painted "_s, String::fromUTF8(name)));
    return 0;
}
