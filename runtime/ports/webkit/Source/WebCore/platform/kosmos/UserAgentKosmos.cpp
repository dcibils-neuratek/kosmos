/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **What JetSki says it is** (`docs/w7.md`): GTK's user agent, in its
 * shape and its words - `Mozilla/5.0 (X11; Kosmos x86_64)
 * AppleWebKit/605.1.15 (KHTML, like Gecko) Version/60.5 Safari/605.1.15`.
 * WebKit's own warning about this string stands: sites decide what to
 * serve by it, and the shape GTK's port sends is one they serve a desktop
 * browser. Only the system's name is Kosmos's.
 *
 * No site is given another string yet. WebKit's quirks for the sites that
 * need one (`platform/glib/UserAgentQuirks.cpp`) arrive with W7d.
 */

#include "config.h"
#include "UserAgent.h"

#include "HTTPParsers.h"
#include <wtf/NeverDestroyed.h>
#include <wtf/text/MakeString.h>

namespace WebCore {

static ASCIILiteral machineForUAString()
{
#if CPU(X86_64)
    return "x86_64"_s;
#elif CPU(ARM64)
    return "aarch64"_s;
#endif
}

static const String& standardUserAgentStatic()
{
    static NeverDestroyed<const String> userAgent(makeString("Mozilla/5.0 (X11; Kosmos "_s, machineForUAString(),
        ") AppleWebKit/605.1.15 (KHTML, like Gecko) Version/60.5 Safari/605.1.15"_s));
    return userAgent;
}

String standardUserAgent(const String& applicationName, const String& applicationVersion)
{
    if (applicationName.isEmpty())
        return standardUserAgentStatic();
    auto userAgent = makeString(standardUserAgentStatic(), ' ', applicationName, '/',
        applicationVersion.isEmpty() ? "605.1.15"_s : applicationVersion);
    RELEASE_ASSERT_WITH_MESSAGE(isValidHTTPHeaderValue(userAgent), "%s is not a valid user agent header", userAgent.utf8().data());
    return userAgent;
}

String standardUserAgentForURL(const URL&)
{
    return String();
}

} // namespace WebCore
