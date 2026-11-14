/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **W5f's formats, held to this Mac** (`docs/w5.md`): Brotli, Google's
 * woff2, and libavif on dav1d, each given test files and asked what is in
 * them. One line a file, and every line is what the same file says
 * compiled natively on this Mac against the same releases
 * (`tools/run_formats.py`) - so the guest is held to a reference it did
 * not make. What is asked:
 *
 * - **brotli**: each of the release's own `tests/testdata` files that has
 *   a `.compressed` twin, decoded by the streaming decoder fed 512 bytes at
 *   a time, as a page arrives, and compared with the original.
 * - **woff2**: each WOFF2 font turned back into the TrueType it was made
 *   from, its size and fingerprint - the fonts the repository carries,
 *   compressed on this Mac by woff2's own compressor, since its release
 *   ships none.
 * - **avif**: each of libavif's own `tests/data` pictures decoded, through
 *   dav1d, to RGBA at its depth, every frame of an animated one; the
 *   pixels' fingerprint, or the sentence it is refused with.
 *
 * A fingerprint is FNV-1a over 64 bits. C++, because woff2's interface is;
 * it reads its files with `fopen`, so it links the POSIX layer.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include <avif/avif.h>
#include <brotli/decode.h>
#include <dav1d/dav1d.h>
#include <woff2/decode.h>
#include <woff2/output.h>

#include "formattest.h"

static formattest_say_fn say_fn;
static void *say_ctx;

static void say(const char *format, ...)
{
    char line[1024];
    va_list ap;

    va_start(ap, format);
    vsnprintf(line, sizeof line, format, ap);
    va_end(ap);
    if (say_fn)
        say_fn(line, say_ctx);
    else
        puts(line);
}

static uint64_t fnv(uint64_t h, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static const uint64_t FNV_START = 0xcbf29ce484222325ull;

static bool slurp(const std::string &path, std::vector<uint8_t> &out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    uint8_t chunk[65536];
    size_t n;
    out.clear();
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
        out.insert(out.end(), chunk, chunk + n);
    fclose(f);
    return true;
}

/* ---- brotli ---- */

static void brotli_one(const std::string &folder, const std::string &name)
{
    std::vector<uint8_t> packed, original;
    std::string plain = name.substr(0, name.size() - strlen(".compressed"));

    if (!slurp(folder + "/" + name, packed) || !slurp(folder + "/" + plain, original)) {
        say("brotli %s: not read", name.c_str());
        return;
    }

    BrotliDecoderState *s = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
    std::vector<uint8_t> out;
    uint8_t buffer[4096];
    size_t at = 0;
    BrotliDecoderResult r = BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT;

    while (r != BROTLI_DECODER_RESULT_SUCCESS && r != BROTLI_DECODER_RESULT_ERROR) {
        size_t step = packed.size() - at < 512 ? packed.size() - at : 512;
        if (r == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT && step == 0)
            break;
        size_t in_left = r == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT ? step : 0;
        const uint8_t *in = packed.data() + at;
        do {
            size_t out_left = sizeof buffer;
            uint8_t *o = buffer;
            r = BrotliDecoderDecompressStream(s, &in_left, &in, &out_left, &o, nullptr);
            out.insert(out.end(), buffer, o);
        } while (r == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT);
        at = static_cast<size_t>(in - packed.data());
    }

    const char *error = r == BROTLI_DECODER_RESULT_SUCCESS ? nullptr
        : BrotliDecoderErrorString(BrotliDecoderGetErrorCode(s));
    BrotliDecoderDestroyInstance(s);
    if (error) {
        say("brotli %s: %s", name.c_str(), error);
        return;
    }
    bool same = out == original;
    say("brotli %s: %zu bytes %016llx %s", name.c_str(), out.size(),
        (unsigned long long)fnv(FNV_START, out.data(), out.size()),
        same ? "as the original" : "NOT the original");
}

/* ---- woff2 ---- */

static void woff2_one(const std::string &folder, const std::string &name)
{
    std::vector<uint8_t> packed;

    if (!slurp(folder + "/" + name, packed)) {
        say("woff2 %s: not read", name.c_str());
        return;
    }
    size_t size = woff2::ComputeWOFF2FinalSize(packed.data(), packed.size());
    std::string font;
    woff2::WOFF2StringOut sink(&font);
    sink.SetMaxSize(size < (64u << 20) ? 64u << 20 : size);
    if (!woff2::ConvertWOFF2ToTTF(packed.data(), packed.size(), &sink)) {
        say("woff2 %s: refused", name.c_str());
        return;
    }
    font.resize(sink.Size());
    say("woff2 %s: %zu bytes (said %zu) %016llx", name.c_str(), font.size(), size,
        (unsigned long long)fnv(FNV_START, reinterpret_cast<const uint8_t *>(font.data()),
                                font.size()));
}

/* ---- avif ---- */

static void avif_one(const std::string &folder, const std::string &name)
{
    std::vector<uint8_t> bytes;

    if (!slurp(folder + "/" + name, bytes)) {
        say("avif %s: not read", name.c_str());
        return;
    }

    avifDecoder *d = avifDecoderCreate();
    // One thread on both sides, so nothing about the answer depends on how
    // a machine schedules dav1d's; and the codec named, so a build that
    // lost dav1d is refused rather than quietly choosing another.
    d->maxThreads = 1;
    d->codecChoice = AVIF_CODEC_CHOICE_DAV1D;
    avifResult r = avifDecoderSetIOMemory(d, bytes.data(), bytes.size());
    if (r == AVIF_RESULT_OK)
        r = avifDecoderParse(d);
    if (r != AVIF_RESULT_OK) {
        say("avif %s: %s", name.c_str(), avifResultToString(r));
        avifDecoderDestroy(d);
        return;
    }

    uint64_t h = FNV_START;
    int frames = 0;
    while ((r = avifDecoderNextImage(d)) == AVIF_RESULT_OK) {
        avifRGBImage rgb;
        avifRGBImageSetDefaults(&rgb, d->image);
        rgb.format = AVIF_RGB_FORMAT_RGBA;
        rgb.depth = d->image->depth > 8 ? 16 : 8;
        if (avifRGBImageAllocatePixels(&rgb) != AVIF_RESULT_OK) {
            r = AVIF_RESULT_OUT_OF_MEMORY;
            break;
        }
        r = avifImageYUVToRGB(d->image, &rgb);
        if (r == AVIF_RESULT_OK)
            for (uint32_t y = 0; y < rgb.height; y++)
                h = fnv(h, rgb.pixels + (size_t)y * rgb.rowBytes,
                        (size_t)rgb.width * avifRGBImagePixelSize(&rgb));
        avifRGBImageFreePixels(&rgb);
        if (r != AVIF_RESULT_OK)
            break;
        frames++;
    }

    if (r == AVIF_RESULT_NO_IMAGES_REMAINING && frames > 0)
        say("avif %s: %ux%u, %u bits, %d frame%s, %016llx", name.c_str(), d->image->width,
            d->image->height, d->image->depth, frames, frames == 1 ? "" : "s",
            (unsigned long long)h);
    else
        say("avif %s: %s after %d frames", name.c_str(), avifResultToString(r), frames);
    avifDecoderDestroy(d);
}

extern "C" int formattest_run(const char *folder, formattest_say_fn fn, void *ctx)
{
    say_fn = fn;
    say_ctx = ctx;

    say("versions: brotli %u.%u.%u, avif %s, dav1d %s",
        BrotliDecoderVersion() >> 24, (BrotliDecoderVersion() >> 12) & 0xfff,
        BrotliDecoderVersion() & 0xfff, avifVersion(), dav1d_version());

    std::vector<uint8_t> manifest;
    if (!slurp(std::string(folder) + "/manifest.txt", manifest)) {
        say("formattest: no manifest in %s", folder);
        return -1;
    }

    int files = 0;
    std::string all(manifest.begin(), manifest.end());
    size_t at = 0;
    while (at < all.size()) {
        size_t end = all.find('\n', at);
        if (end == std::string::npos)
            end = all.size();
        std::string name = all.substr(at, end - at);
        at = end + 1;
        if (name.rfind("brotli/", 0) == 0 && name.size() > 11
            && name.compare(name.size() - 11, 11, ".compressed") == 0)
            brotli_one(folder, name);
        else if (name.rfind("woff2/", 0) == 0)
            woff2_one(folder, name);
        else if (name.rfind("avif/", 0) == 0)
            avif_one(folder, name);
        else
            continue;
        files++;
    }
    say("formattest: %d files", files);
    return 0;
}

#ifdef FORMATTEST_HOST
int main(int argc, char **argv)
{
    return argc == 2 ? formattest_run(argv[1], nullptr, nullptr) : 2;
}
#endif
