/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/* Where WebCore's fonts are on Kosmos: FontFolderKosmos.h. */

#include "config.h"
#include "FontFolderKosmos.h"

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

} // namespace WebCore
