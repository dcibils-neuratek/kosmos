/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * What a file's extension says it is, for a page opened from a file
 * (`docs/webkit.html`, *Save page as*). The PlayStation port's table, which
 * this port used until a saved page had to open again: the same pairs, and
 * `mhtml` and `mht` as `multipart/related` - an MHTML archive, which WebKit
 * opens as the page it holds (`MIMETypeRegistry::isWebArchiveMIMEType`).
 * Without them a saved page opened as text.
 */

#include "config.h"
#include "MIMETypeRegistry.h"

namespace WebCore {

static const std::initializer_list<TypeExtensionPair>& platformMediaTypes()
{
    static std::initializer_list<TypeExtensionPair> types = {
        { "image/bmp"_s, "bmp"_s },
        { "text/css"_s, "css"_s },
        { "image/gif"_s, "gif"_s },
        { "text/html"_s, "htm"_s },
        { "text/html"_s, "html"_s },
        { "image/x-icon"_s, "ico"_s },
        { "image/jpeg"_s, "jpeg"_s },
        { "image/jpeg"_s, "jpg"_s },
        { "application/x-javascript"_s, "js"_s },
        { "multipart/related"_s, "mht"_s },
        { "multipart/related"_s, "mhtml"_s },
        { "application/pdf"_s, "pdf"_s },
        { "image/png"_s, "png"_s },
        { "application/rss+xml"_s, "rss"_s },
        { "image/svg+xml"_s, "svg"_s },
        { "text/plain"_s, "text"_s },
        { "text/plain"_s, "txt"_s },
        { "image/webp"_s, "webp"_s },
        { "application/xhtml+xml"_s, "xhtml"_s },
        { "text/xml"_s, "xml"_s },
        { "text/xsl"_s, "xsl"_s },
    };
    return types;
}

String MIMETypeRegistry::mimeTypeForExtension(StringView extension)
{
    for (auto& entry : platformMediaTypes()) {
        if (equalIgnoringASCIICase(extension, entry.extension))
            return entry.type;
    }
    return emptyString();
}

bool MIMETypeRegistry::isApplicationPluginMIMEType(const String&)
{
    return false;
}

String MIMETypeRegistry::preferredExtensionForMIMEType(const String& mimeType)
{
    for (auto& entry : platformMediaTypes()) {
        if (equalIgnoringASCIICase(mimeType, entry.type))
            return entry.extension;
    }
    return emptyString();
}

Vector<String> MIMETypeRegistry::extensionsForMIMEType(const String& mimeType)
{
    Vector<String> found;
    for (auto& entry : platformMediaTypes()) {
        if (equalIgnoringASCIICase(mimeType, entry.type))
            found.append(entry.extension);
    }
    return found;
}

} // namespace WebCore
