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
#include <WebCore/DocumentLoader.h>
#include <WebCore/DocumentWriter.h>
#include <WebCore/EmptyClients.h>
#include <WebCore/FontFolderKosmos.h>
#include <WebCore/FrameLoader.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/LocalFrameView.h>
#include <WebCore/LoaderStrategy.h>
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
#include <wtf/text/MakeString.h>
#include <wtf/text/StringView.h>

#include <stdio.h>

using namespace WebCore;

static webcoretest_say_fn g_say;
static void* g_ctx;

static void say(const String& line)
{
    g_say(line.utf8().data(), g_ctx);
}

// One page, from its file, laid out; its render tree said a line at a time.
static bool treeOf(const String& folder, const String& name)
{
    auto bytes = FileSystem::readEntireFile(makeString(folder, "/pages/"_s, name));
    if (!bytes) {
        say(makeString("webcoretest: no page "_s, name));
        return false;
    }

    Ref page = Page::create(pageConfigurationWithEmptyClients(std::nullopt, PAL::SessionID::defaultSessionID()));
    page->settings().setAcceleratedCompositingEnabled(false);
    page->settings().setScriptEnabled(true);

    RefPtr frame = page->localMainFrame();
    if (!frame) {
        say("webcoretest: a page with no frame"_s);
        return false;
    }
    frame->setView(LocalFrameView::create(*frame, IntSize { 800, 600 }));
    frame->init();

    RefPtr loader = frame->loader().activeDocumentLoader();
    auto& writer = loader->writer();
    writer.setMIMEType("text/html"_s);
    writer.begin();
    writer.insertDataSynchronously(String::fromUTF8(bytes->span()));
    writer.end();

    RefPtr document = frame->document();
    if (!document) {
        say(makeString("webcoretest: no document for "_s, name));
        return false;
    }
    document->updateLayoutIgnorePendingStylesheets();

    say(makeString("page "_s, name));
    auto tree = externalRepresentation(frame.get());
    for (auto line : StringView(tree).split('\n'))
        say(makeString("tree "_s, line));
    return true;
}

/*
 * **The strategies a page asks the program that made it for** - how to
 * load, play media and hold blobs. WebKit's web process and WebKitLegacy
 * each bring theirs, and neither is in this source; a page here arrives
 * through its document's writer and fetches nothing, so the loader loads
 * nothing and says each refusal is a cancellation. The real loader, curl
 * through WebCore's ResourceHandle as WebKitLegacy's does it, is W6d's
 * (`docs/w6.md`).
 */
namespace {

class NoLoader final : public LoaderStrategy {
public:
    void loadResource(LocalFrame&, CachedResource&, ResourceRequest&&, const ResourceLoaderOptions&,
        CompletionHandler<void(RefPtr<SubresourceLoader>&&)>&& done) final { done(nullptr); }
    void loadResourceSynchronously(FrameLoader&, ResourceLoaderIdentifier, const ResourceRequest& request,
        ClientCredentialPolicy, const FetchOptions&, const HTTPHeaderMap&, ResourceError& error,
        ResourceResponse&, Vector<uint8_t>&) final { error = cancelledError(request); }
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
    LoaderStrategy* createLoaderStrategy() final { return new NoLoader; }
    PasteboardStrategy* createPasteboardStrategy() final { return nullptr; }
    MediaStrategy* createMediaStrategy() final { return new NoMedia; }
    BlobRegistry* createBlobRegistry() final { return new NoBlobs; }
};

} // namespace

extern "C" int webcoretest_run(const char* folder, const char* const* pages, int count,
                               webcoretest_say_fn fn, void* ctx)
{
    g_say = fn;
    g_ctx = ctx;

    static bool started = [] {
        WTF::initializeMainThread();
        JSC::initialize();
        static NeverDestroyed<Strategies> strategies;
        setPlatformStrategies(&strategies.get());
        ProcessCapabilities::setCanUseAcceleratedBuffers(false);
        return true;
    }();
    (void)started;

    auto fonts = makeString(String::fromUTF8(folder), "/fonts/"_s);
    setKosmosFontFolder(fonts.utf8().data());

    int made = 0;
    for (int i = 0; i < count; i++) {
        if (treeOf(String::fromUTF8(folder), String::fromUTF8(pages[i])))
            made++;
    }
    say(makeString("webcoretest: "_s, made, " of "_s, count, " pages"_s));
    return made == count ? made : -1;
}
