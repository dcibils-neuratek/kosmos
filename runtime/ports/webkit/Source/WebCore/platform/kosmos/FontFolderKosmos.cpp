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

SkString kosmosFontFamily(SkFontMgr& manager, const char* name)
{
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
