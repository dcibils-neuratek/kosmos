/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **Whom curl trusts, in WebKit's network process** (`docs/w7.md` W7b).
 *
 * Two sources, one list handed to curl as PEM (`CURLOPT_CAINFO_BLOB`):
 *
 * - **Mozilla's roots**, built into the image (`tools/roots2c.py`), from
 *   `assets/ca/cacert.pem` - the file the TLS Kit's anchors are made from,
 *   so the browser and the rest of Kosmos trust the same authorities.
 * - **Authorities the person added**: every `.pem` file in
 *   `/Home/Settings/JetSki/Certificates/`, read once as the process starts.
 *   How a server of one's own is trusted - and how `x86-sites` has its
 *   test authority trusted without anything built in knowing about it.
 *
 * curl itself was built to look for no bundle (`CURL_CA_BUNDLE=none`), so
 * these are all it has: a site whose chain ends anywhere else is refused.
 */

#include "config.h"
#include "CurlSSLHandle.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>

extern "C" {
extern const unsigned char kosmos_ca_roots[];
extern const unsigned long kosmos_ca_roots_size;
}

namespace WebCore {

static constexpr const char* addedCertificates = "/Home/Settings/JetSki/Certificates";

// One file's bytes onto the end of the list, with a newline between it and
// the next, as PEM allows.
static void appendFile(Vector<uint8_t>& list, const char* path)
{
    FILE* file = fopen(path, "rb");

    if (!file)
        return;

    unsigned char buffer[4096];
    size_t got;
    size_t before = list.size();

    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0)
        list.append(std::span<const uint8_t>(buffer, got));

    fclose(file);

    if (list.size() > before)
        list.append('\n');
}

void CurlSSLHandle::platformInitialize()
{
    Vector<uint8_t> list;

    list.append(std::span<const uint8_t>(kosmos_ca_roots, kosmos_ca_roots_size));
    list.append('\n');

    unsigned added = 0;

    if (DIR* folder = opendir(addedCertificates)) {
        while (struct dirent* entry = readdir(folder)) {
            size_t length = strlen(entry->d_name);

            if (length < 5 || strcmp(entry->d_name + length - 4, ".pem"))
                continue;

            char path[512];

            if (snprintf(path, sizeof(path), "%s/%s", addedCertificates, entry->d_name) >= static_cast<int>(sizeof(path)))
                continue;

            appendFile(list, path);
            added++;
        }

        closedir(folder);
    }

    if (added)
        WTFLogAlways("JetSki: %u certificate file%s added to Mozilla's roots, from %s", added, added == 1 ? "" : "s", addedCertificates);

    setCACertData(WTF::move(list));
}

} // namespace WebCore
