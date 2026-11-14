/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The UI process's page, its platform hooks** (`docs/w7.md` W7a):
 * PlayStation's, without its view backend - the user agent WebCore's
 * standard one, recent searches not kept yet, and nothing done when the
 * editor's state changes until there is a window to show it in (W7c).
 */

#include "config.h"
#include "WebPageProxy.h"

#include "EditorState.h"
#include <WebCore/NotImplemented.h>
#include <WebCore/SearchPopupMenu.h>
#include <WebCore/UserAgent.h>

namespace WebKit {

void WebPageProxy::platformInitialize()
{
}

String WebPageProxy::userAgentForURL(const URL&)
{
    return userAgent();
}

String WebPageProxy::standardUserAgent(const String& applicationNameForUserAgent)
{
    return WebCore::standardUserAgent(applicationNameForUserAgent);
}

void WebPageProxy::saveRecentSearches(IPC::Connection&, const String&, const Vector<WebCore::RecentSearch>&)
{
    notImplemented();
}

void WebPageProxy::loadRecentSearches(IPC::Connection&, const String&, CompletionHandler<void(Vector<WebCore::RecentSearch>&&)>&& completionHandler)
{
    notImplemented();
    completionHandler({ });
}

void WebPageProxy::didUpdateEditorState(const EditorState&, const EditorState&)
{
}

} // namespace WebKit
