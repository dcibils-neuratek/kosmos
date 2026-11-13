/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/* Where WebCore's fonts are on Kosmos: FontFolderKosmos.h. */

#include "config.h"
#include "FontFolderKosmos.h"

#include <wtf/ASCIICType.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/CString.h>

namespace WebCore {

static CString& folder()
{
    static NeverDestroyed<CString> value { "/Kosmos/Fonts/" };
    return value.get();
}

const char* kosmosFontFolder()
{
    return folder().data();
}

void setKosmosFontFolder(const char* path)
{
    folder() = CString(path);
}

static bool sameIgnoringASCIICase(const char* a, const char* b)
{
    for (; *a && *b; a++, b++) {
        if (toASCIILower(*a) != toASCIILower(*b))
            return false;
    }
    return *a == *b;
}

// The generic families CSS names, as Kosmos's fonts: what Fontconfig
// decides for the other ports. A page that names no font is in `serif`
// (WebKit's standard family), and so is the last resort.
static const char* kosmosGenericFamily(const char* name)
{
    static const struct {
        const char* generic;
        const char* family;
    } generics[] = {
        { "serif", "IBM Plex Serif" },
        { "ui-serif", "IBM Plex Serif" },
        { "sans-serif", "IBM Plex Sans" },
        { "ui-sans-serif", "IBM Plex Sans" },
        { "system-ui", "IBM Plex Sans" },
        { "cursive", "IBM Plex Serif" },
        { "fantasy", "IBM Plex Sans" },
        { "monospace", "IBM Plex Mono" },
        { "ui-monospace", "IBM Plex Mono" },
    };
    for (const auto& generic : generics) {
        if (sameIgnoringASCIICase(name, generic.generic))
            return generic.family;
    }
    return name;
}

SkString kosmosFontFamily(SkFontMgr& manager, const char* asked)
{
    const char* name = kosmosGenericFamily(asked);
    int count = manager.countFamilies();
    for (int i = 0; i < count; i++) {
        SkString family;
        manager.getFamilyName(i, &family);
        if (sameIgnoringASCIICase(family.c_str(), name))
            return family;
    }
    return SkString(name);
}

} // namespace WebCore
