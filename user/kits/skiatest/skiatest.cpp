/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **W5e's Skia test** (`docs/w5.md`, `tools/run_skia.py`): Skia, as
 * WebKitGTK 2.54.2 carries it, drawing on the processor - paths, strokes and
 * dashes, every kind of gradient, text through FreeType from a folder of
 * fonts, pictures decoded by Skia's own PNG and JPEG codecs and drawn
 * with each kind of sampling, layers, blend modes, clips, blurs, a colour
 * matrix, a recorded picture played back, and a destination in another
 * colour space.
 *
 * Each scene is drawn into a surface of its own, with its pixel format
 * named rather than the platform's, and said as one line: its size, a
 * 64-bit FNV-1a of its pixels row by row, and how many pixels differ from
 * the ground it was cleared to. The same file compiled on this Mac, for
 * x86-64 and run under Rosetta, says what the lines must be - the same
 * Skia, the same vector paths (`-march=x86-64` on both sides), the same
 * FreeType - so one pixel drawn differently is a line that disagrees.
 */

#include "skia_defines.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "include/codec/SkCodec.h"
#include "include/codec/SkJpegDecoder.h"
#include "include/codec/SkPngDecoder.h"
#include "include/core/SkBlendMode.h"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageFilter.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkMilestone.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPathEffect.h"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkRRect.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkString.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTypeface.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/effects/SkGradient.h"
#include "include/effects/SkImageFilters.h"
#include "include/ports/SkFontMgr_directory.h"

#include "skiatest.h"

#include <ft2build.h>
#include FT_FREETYPE_H

namespace {

constexpr int W = 320;
constexpr int H = 240;
constexpr SkColor GROUND = 0xFFF4F1E8;

skiatest_say_fn g_say;
void *g_ctx;

void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void say(const char *fmt, ...)
{
    char line[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    g_say(line, g_ctx);
}

/* The surface every scene is drawn into: RGBA, premultiplied, named
 * rather than the platform's N32, with no colour space unless the scene
 * asks for one - so nothing is converted that the scene did not say. */
sk_sp<SkSurface> surface(sk_sp<SkColorSpace> cs = nullptr)
{
    return SkSurfaces::Raster(SkImageInfo::Make(W, H, kRGBA_8888_SkColorType,
                                                kPremul_SkAlphaType, std::move(cs)));
}

uint64_t fnv(const uint8_t *p, size_t n, uint64_t h)
{
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* The line a scene is: its pixels read back row by row, without the
 * padding a row may carry. */
int said = 0;

void finish(const char *name, SkSurface *s)
{
    SkPixmap pm;
    uint64_t h = 0xcbf29ce484222325ULL;
    unsigned painted = 0;
    uint32_t ground;

    if (!s->peekPixels(&pm)) {
        say("scene: %s cannot be read", name);
        return;
    }

    {
        uint8_t g[4] = { (uint8_t)SkColorGetR(GROUND), (uint8_t)SkColorGetG(GROUND),
                         (uint8_t)SkColorGetB(GROUND), (uint8_t)SkColorGetA(GROUND) };
        memcpy(&ground, g, 4);
    }

    for (int y = 0; y < pm.height(); y++) {
        const uint8_t *row = (const uint8_t *)pm.addr(0, y);
        h = fnv(row, (size_t)pm.width() * 4, h);
        for (int x = 0; x < pm.width(); x++) {
            uint32_t px;
            memcpy(&px, row + x * 4, 4);
            painted += px != ground;
        }
    }
    say("scene: %s %dx%d fnv %016llx painted %u", name, pm.width(), pm.height(),
        (unsigned long long)h, painted);
    said++;

#ifdef SKIATEST_HOST
    /* On this Mac, for a person to look at: each scene as a PPM in the
     * folder SKIATEST_PICTURES names. Every scene is opaque, cleared to
     * the ground first, so its colour is its premultiplied colour. */
    if (const char *dir = getenv("SKIATEST_PICTURES")) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
        if (FILE *f = fopen(path, "wb")) {
            fprintf(f, "P6\n%d %d\n255\n", pm.width(), pm.height());
            for (int y = 0; y < pm.height(); y++) {
                const uint8_t *row = (const uint8_t *)pm.addr(0, y);
                for (int x = 0; x < pm.width(); x++) {
                    fwrite(row + x * 4, 1, 3, f);
                }
            }
            fclose(f);
        }
    }
#endif
}

SkPath star(float cx, float cy, float r, int points)
{
    SkPathBuilder b;

    for (int i = 0; i < points; i++) {
        /* Every second vertex of a 2n-gon, as a pentagram is drawn. */
        float a = (float)i * 2.0f * (float)M_PI * (float)(points / 2) / (float)points
                  - (float)M_PI / 2.0f;
        if (i == 0) {
            b.moveTo(cx + r * cosf(a), cy + r * sinf(a));
        } else {
            b.lineTo(cx + r * cosf(a), cy + r * sinf(a));
        }
    }
    b.close();
    return b.detach();
}

void scene_fills()
{
    sk_sp<SkSurface> s = surface();
    SkCanvas *c = s->getCanvas();
    SkPaint p;

    c->clear(GROUND);
    p.setAntiAlias(true);

    SkPath winding = star(60, 70, 50, 5);
    p.setColor(0xFF2B6CB0);
    c->drawPath(winding, p);

    SkPathBuilder eo;
    eo.addPath(star(170, 70, 50, 5));
    eo.setFillType(SkPathFillType::kEvenOdd);
    p.setColor(0xFFC05621);
    c->drawPath(eo.detach(), p);

    SkPathBuilder curves;
    curves.moveTo(230, 120)
          .quadTo(260, 20, 300, 110)
          .cubicTo(320, 160, 250, 200, 240, 150)
          .conicTo(225, 200, 200, 140, 0.6f)
          .close();
    p.setColor(0xCC2F855A);
    c->drawPath(curves.detach(), p);

    c->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(20, 140, 120, 80), 18, 12), p);
    p.setColor(0x806B46C1);
    c->drawOval(SkRect::MakeXYWH(80, 130, 140, 90), p);

    /* Not antialiased, and under a rotation: the scan converter's other half. */
    p.setAntiAlias(false);
    p.setColor(0xFF1A202C);
    c->save();
    c->rotate(17, 160, 120);
    c->drawRect(SkRect::MakeXYWH(140, 100, 40, 40), p);
    c->restore();
    finish("fills", s.get());
}

void scene_strokes()
{
    sk_sp<SkSurface> s = surface();
    SkCanvas *c = s->getCanvas();
    SkPaint p;
    const SkPaint::Join joins[] = { SkPaint::kMiter_Join, SkPaint::kRound_Join,
                                    SkPaint::kBevel_Join };
    const SkPaint::Cap caps[] = { SkPaint::kButt_Cap, SkPaint::kRound_Cap,
                                  SkPaint::kSquare_Cap };

    c->clear(GROUND);
    p.setAntiAlias(true);
    p.setStyle(SkPaint::kStroke_Style);
    p.setStrokeWidth(10);
    p.setColor(0xFF2C5282);

    for (int i = 0; i < 3; i++) {
        SkPathBuilder b;
        b.moveTo(20 + 100 * i, 80).lineTo(60 + 100 * i, 20).lineTo(90 + 100 * i, 80);
        p.setStrokeJoin(joins[i]);
        p.setStrokeCap(caps[i]);
        c->drawPath(b.detach(), p);
    }

    /* Dashes along a circle and a curve. */
    const SkScalar intervals[] = { 14, 6, 3, 6 };
    p.setPathEffect(SkDashPathEffect::Make(intervals, 4));
    p.setStrokeWidth(4);
    p.setColor(0xFF9C4221);
    c->drawCircle(70, 165, 50, p);
    SkPathBuilder wave;
    wave.moveTo(140, 160).cubicTo(180, 100, 220, 220, 300, 150);
    c->drawPath(wave.detach(), p);
    p.setPathEffect(nullptr);

    /* Hairlines, and a stroke that thin under a scale. */
    p.setStrokeWidth(0);
    p.setColor(0xFF000000);
    for (int i = 0; i < 12; i++) {
        c->drawLine(140, 200 + i * 3, 310, 190 + i * 4, p);
    }
    c->save();
    c->scale(3.5f, 1.25f);
    p.setStrokeWidth(0.5f);
    c->drawCircle(70, 180, 8, p);
    c->restore();
    finish("strokes", s.get());
}

void scene_gradients()
{
    sk_sp<SkSurface> s = surface();
    SkCanvas *c = s->getCanvas();
    SkPaint p;
    const SkColor4f colors[] = { { 0.17f, 0.42f, 0.69f, 1 }, { 0.95f, 0.85f, 0.30f, 1 },
                                 { 0.75f, 0.20f, 0.13f, 0.6f } };
    const float stops[] = { 0, 0.4f, 1 };
    const SkTileMode modes[] = { SkTileMode::kClamp, SkTileMode::kRepeat,
                                 SkTileMode::kMirror, SkTileMode::kDecal };

    c->clear(GROUND);
    p.setAntiAlias(true);

    for (int i = 0; i < 4; i++) {
        SkPoint pts[2] = { { 10.0f + 80 * i, 20 }, { 40.0f + 80 * i, 40 } };
        SkGradient g(SkGradient::Colors(colors, stops, modes[i]), {});
        p.setShader(SkShaders::LinearGradient(pts, g));
        c->drawRect(SkRect::MakeXYWH(5 + 80 * i, 5, 70, 70), p);
    }

    SkGradient radial(SkGradient::Colors(colors, stops, SkTileMode::kMirror), {});
    p.setShader(SkShaders::RadialGradient({ 60, 150 }, 30, radial));
    c->drawCircle(60, 150, 55, p);

    SkGradient sweep(SkGradient::Colors(colors, SkTileMode::kClamp), {});
    p.setShader(SkShaders::SweepGradient({ 160, 150 }, 30, 300, sweep));
    c->drawCircle(160, 150, 55, p);

    SkGradient conical(SkGradient::Colors(colors, stops, SkTileMode::kClamp), {});
    p.setShader(SkShaders::TwoPointConicalGradient({ 250, 130 }, 10, { 265, 160 }, 55,
                                                   conical));
    c->drawRect(SkRect::MakeXYWH(200, 90, 115, 145), p);

    /* Interpolated in another space, premultiplied: what CSS asks for. */
    SkGradient::Interpolation oklab;
    oklab.fColorSpace = SkGradient::Interpolation::ColorSpace::kOKLab;
    oklab.fInPremul = SkGradient::Interpolation::InPremul::kYes;
    SkPoint across[2] = { { 0, 220 }, { 320, 220 } };
    p.setShader(SkShaders::LinearGradient(across,
                SkGradient(SkGradient::Colors(colors, stops, SkTileMode::kClamp), oklab)));
    c->drawRect(SkRect::MakeXYWH(0, 212, 200, 28), p);
    finish("gradients", s.get());
}

sk_sp<SkFontMgr> g_fonts;

/* Each family the folder's fonts make, by name: the order a folder is
 * listed in is the file system's, and not Skia's to agree on. */
void fonts_said()
{
    std::vector<std::pair<std::string, int>> found;

    for (int i = 0; i < g_fonts->countFamilies(); i++) {
        SkString name;
        g_fonts->getFamilyName(i, &name);
        sk_sp<SkFontStyleSet> set = g_fonts->createStyleSet(i);
        found.emplace_back(name.c_str(), set ? set->count() : -1);
    }
    std::sort(found.begin(), found.end());
    for (const auto &[name, styles] : found) {
        say("family: %s, %d styles", name.c_str(), styles);
    }
}

void scene_text()
{
    sk_sp<SkSurface> s = surface();
    SkCanvas *c = s->getCanvas();
    SkPaint p;
    sk_sp<SkTypeface> sans = g_fonts->matchFamilyStyle("IBM Plex Sans", SkFontStyle::Normal());
    sk_sp<SkTypeface> bold = g_fonts->matchFamilyStyle("IBM Plex Sans", SkFontStyle::Bold());
    sk_sp<SkTypeface> italic = g_fonts->matchFamilyStyle("IBM Plex Sans", SkFontStyle::Italic());
    sk_sp<SkTypeface> mono = g_fonts->matchFamilyStyle("IBM Plex Mono", SkFontStyle::Normal());
    const char *line = "Kosmos draws with Skia: fi fl 0123456789";

    if (!sans || !bold || !italic || !mono) {
        say("scene: text has no fonts (%s %s %s %s)", sans ? "sans" : "-", bold ? "bold" : "-",
            italic ? "italic" : "-", mono ? "mono" : "-");
        return;
    }

    c->clear(GROUND);
    p.setAntiAlias(true);
    p.setColor(0xFF1A202C);

    const float sizes[] = { 9, 12, 16, 24 };
    float y = 14;
    for (float size : sizes) {
        SkFont f(sans, size);
        f.setEdging(SkFont::Edging::kAntiAlias);
        c->drawString(line, 6, y, f, p);
        say("measure: IBM Plex Sans %g %d", (double)size,
            (int)(f.measureText(line, strlen(line), SkTextEncoding::kUTF8) * 64));
        y += size + 6;
    }

    SkFont b(bold, 30);
    p.setColor(0xFF2B6CB0);
    c->drawString("Bold 30", 6, y + 22, b, p);
    SkFont it(italic, 20);
    it.setEdging(SkFont::Edging::kAlias);
    p.setColor(0xFFC05621);
    c->drawString("Italic, aliased", 150, y + 22, it, p);

    SkFont m(mono, 13);
    m.setHinting(SkFontHinting::kFull);
    p.setColor(0xFF2F855A);
    c->drawString("mono: {int x = 0;} // hinted", 6, y + 52, m, p);

    /* A glyph turned and grown: outlines, not a cached bitmap. */
    SkFont big(sans, 80);
    p.setColor(0x806B46C1);
    c->save();
    c->rotate(-12, 240, 200);
    c->drawString("Aa", 200, 230, big, p);
    c->restore();
    finish("text", s.get());
}

struct Picture {
    const char *file;
    bool jpeg;
    sk_sp<SkImage> image;
};

Picture g_pictures[] = {
    { "pngtest.png", false, nullptr },
    { "rgb-alpha-8-sRGB.png", false, nullptr },
    { "gray-16-tRNS.png", false, nullptr },
    { "testorig.jpg", true, nullptr },
    { "testimgint.jpg", true, nullptr },
};

void pictures_decoded(const char *folder)
{
    for (Picture &pic : g_pictures) {
        char path[512];
        snprintf(path, sizeof path, "%s/pictures/%s", folder, pic.file);
        sk_sp<SkData> data = SkData::MakeFromFileName(path);
        if (!data) {
            say("picture: %s cannot be read", pic.file);
            continue;
        }

        SkCodec::Result r = SkCodec::kSuccess;
        std::unique_ptr<SkCodec> codec = pic.jpeg ? SkJpegDecoder::Decode(data, &r)
                                                  : SkPngDecoder::Decode(data, &r);
        if (!codec) {
            say("picture: %s: no codec, %s", pic.file, SkCodec::ResultToString(r));
            continue;
        }

        auto [image, result] = codec->getImage();
        if (!image) {
            say("picture: %s: %s", pic.file, SkCodec::ResultToString(result));
            continue;
        }

        /* Its own pixels, as the codec gave them, read back as RGBA. */
        SkImageInfo info = SkImageInfo::Make(image->width(), image->height(),
                                             kRGBA_8888_SkColorType, kPremul_SkAlphaType);
        std::vector<uint8_t> px(info.computeMinByteSize());
        uint64_t h = 0xcbf29ce484222325ULL;
        if (image->readPixels(info, px.data(), info.minRowBytes(), 0, 0)) {
            h = fnv(px.data(), px.size(), h);
        }
        say("picture: %s %dx%d %s fnv %016llx", pic.file, image->width(), image->height(),
            SkCodec::ResultToString(result), (unsigned long long)h);
        pic.image = image;
    }
}

void scene_pictures()
{
    sk_sp<SkSurface> s = surface();
    SkCanvas *c = s->getCanvas();
    const SkSamplingOptions sampling[] = {
        SkSamplingOptions(),
        SkSamplingOptions(SkFilterMode::kLinear),
        SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear),
        SkSamplingOptions(SkCubicResampler::Mitchell()),
    };

    c->clear(GROUND);
    int i = 0;
    for (Picture &pic : g_pictures) {
        if (!pic.image) {
            continue;
        }
        sk_sp<SkImage> image = pic.image->withDefaultMipmaps();
        for (int k = 0; k < 4; k++) {
            /* Down, and in two directions at once: each sampling met
             * shrinking and stretching. */
            float w = k % 2 ? 30.0f : 76.0f;
            float h = k % 2 ? 44.0f : 22.0f;
            c->drawImageRect(image, SkRect::MakeXYWH(4 + 80.0f * k, 4 + 47.0f * i, w, h),
                             sampling[k], nullptr);
        }
        i++;
    }

    /* And one turned, through a paint with an alpha. */
    for (Picture &pic : g_pictures) {
        if (pic.image) {
            SkPaint p;
            p.setAlphaf(0.7f);
            c->save();
            c->rotate(23, 240, 160);
            c->drawImageRect(pic.image, SkRect::MakeXYWH(190, 120, 100, 80),
                             SkSamplingOptions(SkFilterMode::kLinear), &p);
            c->restore();
            break;
        }
    }
    finish("pictures", s.get());
}

void scene_effects()
{
    sk_sp<SkSurface> s = surface();
    SkCanvas *c = s->getCanvas();
    SkPaint p;
    const SkBlendMode modes[] = { SkBlendMode::kMultiply, SkBlendMode::kScreen,
                                  SkBlendMode::kDifference, SkBlendMode::kXor,
                                  SkBlendMode::kOverlay, SkBlendMode::kColorBurn,
                                  SkBlendMode::kHue, SkBlendMode::kLuminosity };

    c->clear(GROUND);
    p.setAntiAlias(true);

    for (int i = 0; i < 8; i++) {
        float x = 8 + 38.0f * i;
        p.setBlendMode(SkBlendMode::kSrcOver);
        p.setColor(0xFF2B6CB0);
        c->drawRect(SkRect::MakeXYWH(x, 8, 28, 28), p);
        p.setBlendMode(modes[i]);
        p.setColor(0xC0E9A23B);
        c->drawCircle(x + 22, 34, 14, p);
    }
    p.setBlendMode(SkBlendMode::kSrcOver);

    /* A layer with an alpha, holding two shapes that overlap. */
    c->saveLayerAlphaf(nullptr, 0.5f);
    p.setColor(0xFFC05621);
    c->drawCircle(50, 100, 30, p);
    c->drawCircle(80, 100, 30, p);
    c->restore();

    /* An antialiased clip. */
    c->save();
    c->clipPath(star(170, 105, 45, 5), true);
    SkPoint pts[2] = { { 125, 60 }, { 215, 150 } };
    const SkColor4f two[] = { { 0.18f, 0.52f, 0.35f, 1 }, { 0.42f, 0.27f, 0.76f, 1 } };
    p.setShader(SkShaders::LinearGradient(pts, SkGradient(SkGradient::Colors(two,
                                                SkTileMode::kClamp), {})));
    c->drawPaint(p);
    p.setShader(nullptr);
    c->restore();

    /* A blurred mask, a blurred layer, and a shadow. */
    p.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 4));
    p.setColor(0xFF1A202C);
    c->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(240, 70, 60, 60), 10, 10), p);
    p.setMaskFilter(nullptr);

    SkPaint layer;
    layer.setImageFilter(SkImageFilters::Blur(3, 6, nullptr));
    c->saveLayer(nullptr, &layer);
    p.setColor(0xFF2C5282);
    c->drawRect(SkRect::MakeXYWH(20, 160, 80, 60), p);
    c->restore();

    SkPaint shadow;
    shadow.setImageFilter(SkImageFilters::DropShadow(6, 6, 3, 3, { 0, 0, 0, 0.5f }, nullptr,
                                                     nullptr));
    c->saveLayer(nullptr, &shadow);
    p.setColor(0xFFE9A23B);
    c->drawCircle(160, 195, 30, p);
    c->restore();

    /* A colour matrix: sepia, as CSS's filter is. */
    const float sepia[20] = { 0.393f, 0.769f, 0.189f, 0, 0,
                              0.349f, 0.686f, 0.168f, 0, 0,
                              0.272f, 0.534f, 0.131f, 0, 0,
                              0, 0, 0, 1, 0 };
    p.setColorFilter(SkColorFilters::Matrix(sepia));
    p.setColor(0xFF2B6CB0);
    c->drawRect(SkRect::MakeXYWH(230, 160, 80, 70), p);
    finish("effects", s.get());
}

/* The fills drawn into a recording, and the recording played into a
 * surface twice, turned and shrunk: what WebKit's display lists are. */
void scene_recorded()
{
    SkPictureRecorder rec;
    SkCanvas *r = rec.beginRecording(SkRect::MakeWH(W, H));
    SkPaint p;

    p.setAntiAlias(true);
    p.setColor(0xFF2B6CB0);
    r->drawPath(star(160, 120, 100, 7), p);
    p.setColor(0x80C05621);
    r->drawCircle(160, 120, 60, p);
    sk_sp<SkPicture> pic = rec.finishRecordingAsPicture();

    sk_sp<SkSurface> s = surface();
    SkCanvas *c = s->getCanvas();
    c->clear(GROUND);
    SkMatrix half = SkMatrix::Scale(0.5f, 0.5f);
    c->drawPicture(pic, &half, nullptr);
    SkMatrix turned = SkMatrix::RotateDeg(30, { 160, 120 });
    turned.postTranslate(60, 40);
    SkPaint faint;
    faint.setAlphaf(0.6f);
    c->drawPicture(pic, &turned, &faint);
    say("recorded: %d operations", pic->approximateOpCount());
    finish("recorded", s.get());
}

/* A destination in linear sRGB, drawn with sRGB colours and a picture
 * tagged sRGB: every colour goes through skcms. */
void scene_colourspace()
{
    sk_sp<SkSurface> s = surface(SkColorSpace::MakeSRGBLinear());
    SkCanvas *c = s->getCanvas();
    SkPaint p;

    c->clear(GROUND);
    p.setAntiAlias(true);
    p.setColor(0xFF2B6CB0);
    c->drawCircle(80, 80, 60, p);
    p.setColor4f({ 0.9f, 0.4f, 0.1f, 0.5f }, SkColorSpace::MakeSRGB().get());
    c->drawCircle(130, 110, 60, p);
    for (Picture &pic : g_pictures) {
        if (pic.image) {
            c->drawImageRect(pic.image, SkRect::MakeXYWH(190, 20, 120, 200),
                             SkSamplingOptions(SkFilterMode::kLinear), nullptr);
            break;
        }
    }
    finish("colourspace", s.get());
}

} // namespace

extern "C" int skiatest_run(const char *folder, skiatest_say_fn fn, void *ctx)
{
    char fonts[512];
    FT_Library ft;
    FT_Int major, minor, patch;

    g_say = fn;
    g_ctx = ctx;
    said = 0;

    /* Which FreeType Skia's text came from, since a different one is a
     * different picture for the same reason. */
    if (FT_Init_FreeType(&ft) == 0) {
        FT_Library_Version(ft, &major, &minor, &patch);
        FT_Done_FreeType(ft);
        say("versions: skia m%d, freetype %d.%d.%d", SK_MILESTONE, major, minor, patch);
    }

    snprintf(fonts, sizeof fonts, "%s/fonts/", folder);
    g_fonts = SkFontMgr_New_Custom_Directory(fonts);
    if (!g_fonts) {
        say("skiatest: no font manager for %s", fonts);
        return -1;
    }
    fonts_said();
    pictures_decoded(folder);

    scene_fills();
    scene_strokes();
    scene_gradients();
    scene_text();
    scene_pictures();
    scene_effects();
    scene_recorded();
    scene_colourspace();

    for (Picture &pic : g_pictures) {
        pic.image = nullptr;
    }
    g_fonts = nullptr;
    say("skiatest: %d scenes", said);
    return said;
}

#ifdef SKIATEST_HOST
/* On this Mac, the reference: the folder the guest is given, the lines on
 * standard output. */
static void print_line(const char *line, void *ctx)
{
    (void)ctx;
    puts(line);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: skiatest FOLDER\n");
        return 2;
    }
    return skiatest_run(argv[1], print_line, NULL) < 0;
}
#endif
