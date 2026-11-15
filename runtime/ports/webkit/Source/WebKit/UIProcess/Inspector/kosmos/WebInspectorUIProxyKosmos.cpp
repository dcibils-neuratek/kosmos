/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **Web Inspector on Kosmos, the UI process's half** (`docs/webkit.html`,
 * *Developer mode*, agreed 15 November 2026: "Build it as drawn").
 *
 * WebKit's inspector is a web page of its own - Safari's front end,
 * `Main.html` and the rest of `WebInspectorUI` - in a web process of its
 * own, talking to the inspected page through this process. What a port
 * says is where that page's view goes. GTK's makes a widget and packs it
 * beside the inspected one; this makes a view of the kind every tab is
 * (`PlayStationWebView`, as `WKViewCreate` does) and hands it to the
 * application through `WKInspectorSetViewHandlerKosmos`, which draws it
 * beside the page and gives it the pointer and keys over its half. A
 * null view is the inspector gone.
 *
 * It is only ever docked: there are no free-floating windows of
 * WebKit's here, so a "window" opened is the same view, attached.
 *
 * The front end's files are a folder on the disk, set by the application
 * (`WKInspectorSetResourcesKosmos`) and loaded as any page from the disk
 * is: the network process reads them, and patch 0030 lets `Main.html`
 * reach the files beside it.
 */

#include "config.h"
#include "WebInspectorUIProxy.h"

#include "APIPageConfiguration.h"
#include "PlayStationWebView.h"
#include "WKAPICast.h"
#include "WKPageKosmos.h"
#include "WebInspectorUtilities.h"
#include "WebPageGroup.h"
#include "WebPageProxy.h"
#include "WebPreferences.h"
#include "WebProcessPool.h"
#include "WebProcessProxy.h"
#include "WebsiteDataStore.h"
#include <WebCore/NotImplemented.h>
#include <wtf/HashMap.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/MakeString.h>

namespace WebKit {
using namespace WebCore;

namespace {

WKInspectorViewHandlerKosmos viewHandler;
void* viewHandlerContext;

String& resourcesFolder()
{
    static NeverDestroyed<String> folder;
    return folder;
}

MemoryCompactLookupOnlyRobinHoodHashSet<String>& assumedPaths()
{
    static NeverDestroyed<MemoryCompactLookupOnlyRobinHoodHashSet<String>> paths;
    return paths;
}

// Each inspector's view, by the inspector it belongs to. The proxy's own
// member for it is a platform's, and Kosmos is not one of the four the
// header names; a table here keeps the header WebKit's.
HashMap<const WebInspectorUIProxy*, RefPtr<PlayStationWebView>>& views()
{
    static NeverDestroyed<HashMap<const WebInspectorUIProxy*, RefPtr<PlayStationWebView>>> table;
    return table;
}

void tell(WebPageProxy* inspected, PlayStationWebView* view)
{
    if (viewHandler && inspected)
        viewHandler(toAPI(inspected), view ? toAPI(view) : nullptr, viewHandlerContext);
}

}

RefPtr<WebPageProxy> WebInspectorUIProxy::platformCreateFrontendPage()
{
    RefPtr inspected = inspectedPage();
    if (!inspected)
        return nullptr;

    Ref preferences = WebPreferences::create(String(), "WebKit2."_s, "WebKit2."_s);
    preferences->setAllowTopNavigationToDataURLs(true);
    if (m_underTest)
        preferences->setHiddenPageDOMTimerThrottlingEnabled(false);
    // The inspector's own pages are files beside each other in its folder.
    preferences->setAllowFileAccessFromFileURLs(true);

    auto pageGroup = WebPageGroup::create(defaultInspectorPageGroupIdentifierForPage(inspected.get()));
    // Its own store, kept nowhere: the inspector's pages are files and
    // carry no cookies a site could read.
    auto websiteDataStore = WebsiteDataStore::createNonPersistent();
    auto& processPool = defaultInspectorProcessPool(inspectionLevel());

    auto configuration = API::PageConfiguration::create();
    configuration->setProcessPool(&processPool);
    configuration->setPreferences(preferences.ptr());
    configuration->setPageGroup(pageGroup.ptr());
    configuration->setWebsiteDataStore(websiteDataStore.ptr());

    auto view = PlayStationWebView::create(configuration.get());
    if (!view)
        return nullptr;
    RefPtr<WebPageProxy> page = view->page();
    views().set(this, WTF::move(view));
    return page;
}

void WebInspectorUIProxy::platformCreateFrontendWindow()
{
    // No windows of WebKit's own: opened is docked.
    platformAttach();
}

void WebInspectorUIProxy::platformCloseFrontendPageAndWindow()
{
    tell(inspectedPage(), nullptr);
    views().remove(this);
}

void WebInspectorUIProxy::platformDidCloseForCrash()
{
    tell(inspectedPage(), nullptr);
}

void WebInspectorUIProxy::platformInvalidate()
{
    tell(inspectedPage(), nullptr);
    views().remove(this);
}

void WebInspectorUIProxy::platformResetState()
{
}

void WebInspectorUIProxy::platformBringToFront()
{
    platformAttach();
}

void WebInspectorUIProxy::platformBringInspectedPageToFront()
{
}

void WebInspectorUIProxy::platformHide()
{
    tell(inspectedPage(), nullptr);
}

bool WebInspectorUIProxy::platformIsFront()
{
    return views().contains(this);
}

void WebInspectorUIProxy::platformAttachAvailabilityChanged(bool)
{
}

void WebInspectorUIProxy::platformSetForcedAppearance(InspectorFrontendClient::Appearance)
{
}

void WebInspectorUIProxy::platformInspectedURLChanged(const String&)
{
}

void WebInspectorUIProxy::platformShowCertificate(const CertificateInfo&)
{
    notImplemented();
}

String WebInspectorUIProxy::inspectorPageURL()
{
    return makeString("file://"_s, resourcesFolder(), "/Main.html"_s);
}

String WebInspectorUIProxy::inspectorTestPageURL()
{
    return makeString("file://"_s, resourcesFolder(), "/Test.html"_s);
}

DebuggableInfoData WebInspectorUIProxy::infoForLocalDebuggable()
{
    auto data = DebuggableInfoData::empty();
    data.debuggableType = Inspector::DebuggableType::WebPage;
    return data;
}

void WebInspectorUIProxy::platformAttach()
{
    auto found = views().find(this);
    if (found != views().end())
        tell(inspectedPage(), found->value.get());
}

void WebInspectorUIProxy::platformDetach()
{
    tell(inspectedPage(), nullptr);
}

void WebInspectorUIProxy::platformSetAttachedWindowHeight(unsigned)
{
}

void WebInspectorUIProxy::platformSetAttachedWindowWidth(unsigned)
{
}

void WebInspectorUIProxy::platformSetSheetRect(const FloatRect&)
{
}

void WebInspectorUIProxy::platformStartWindowDrag()
{
}

void WebInspectorUIProxy::platformRevealFileExternally(const String&)
{
    notImplemented();
}

void WebInspectorUIProxy::platformSave(Vector<InspectorFrontendClient::SaveData>&&, bool)
{
    notImplemented();
}

void WebInspectorUIProxy::platformLoad(const String&, CompletionHandler<void(const String&)>&& completionHandler)
{
    notImplemented();
    completionHandler(nullString());
}

void WebInspectorUIProxy::platformPickColorFromScreen(CompletionHandler<void(const std::optional<Color>&)>&& completionHandler)
{
    notImplemented();
    completionHandler({ });
}

/*
 * **The paths every web process is taken to read already** (patch 0031):
 * Web Inspector's folder. Its pages are `file:` addresses, and a page of
 * the inspector's is never given a sandbox extension - WebKit asserts it
 * is not - because on Safari its bundle is a path every web process
 * reads. GTK's and WPE's are `resource:` addresses, which never ask.
 */
const MemoryCompactLookupOnlyRobinHoodHashSet<String>& WebProcessProxy::platformPathsWithAssumedReadAccess()
{
    return assumedPaths();
}

} // namespace WebKit

void WKInspectorSetViewHandlerKosmos(WKInspectorViewHandlerKosmos handler, void* context)
{
    WebKit::viewHandler = handler;
    WebKit::viewHandlerContext = context;
}

void WKInspectorSetResourcesKosmos(const char* folder)
{
    WebKit::resourcesFolder() = String::fromUTF8(folder ? folder : "");
    if (!WebKit::resourcesFolder().isEmpty())
        WebKit::assumedPaths().add(makeString(WebKit::resourcesFolder(), "/"_s));
}
