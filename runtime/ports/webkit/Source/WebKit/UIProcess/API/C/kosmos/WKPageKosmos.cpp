/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `WKPagePaintKosmos` (`WKPageKosmos.h`): PlayStation's `WKPagePaint` with
 * a pitch, on Skia alone, and with the background drawn where the drawing
 * area had nothing - PlayStation's draws it only for a colour that cannot
 * be seen.
 *
 * And the pointer, the wheel and keys (`WKPageMouseKosmos` and the rest):
 * WebKit's events made here from numbers, and given to the page through
 * the `NativeWeb*Event` constructors patch 0024 adds for Kosmos.
 */

#include "config.h"
#include "WKPageKosmos.h"

#include "AuthenticationDecisionListener.h"
#include "DrawingAreaProxyCoordinatedGraphics.h"
#include "KosmosFrameTimes.h"
#include "KosmosWake.h"
#include "NativeWebKeyboardEvent.h"
#include "NativeWebMouseEvent.h"
#include "NativeWebWheelEvent.h"
#include "WKAPICast.h"
#include "PageLoadState.h"
#include "WebPageProxy.h"
#include <WebCore/CertificateInfo.h>
#include <WebCore/CertificateSummary.h>
#include <WebCore/Color.h>
#include <WebCore/Region.h>
#include <skia/core/SkCanvas.h>
#include <skia/core/SkSurface.h>
#include <wtf/HexNumber.h>
#include <wtf/MonotonicTime.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>
#include <string.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

static void paintBackground(SkCanvas* canvas, const std::optional<WebCore::Color>& color, const WebCore::IntRect& rect)
{
    SkPaint paint;
    paint.setColor(color ? SkColor(*color) : SK_ColorWHITE);
    canvas->drawRect(SkRect::MakeXYWH(rect.x(), rect.y(), rect.width(), rect.height()), paint);
}

void WKPagePaintKosmos(WKPageRef pageRef, uint32_t* pixels, unsigned width, unsigned height, unsigned pitch, WKRect wkPaintRect)
{
    if (!pixels || !width || !height || pitch < width * 4)
        return;
    auto info = SkImageInfo::MakeN32Premul(width, height, SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::WrapPixels(info, pixels, pitch, nullptr);
    if (!surface)
        return;
    auto* canvas = surface->getCanvas();
    auto paintRect = WebKit::toIntRect(wkPaintRect);
    paintRect.intersect(WebCore::IntRect(0, 0, width, height));

    RefPtr page = WebKit::toImpl(pageRef);
    auto& backgroundColor = page->backgroundColor();
    if (auto* drawingArea = page->drawingArea()) {
        WebCore::Region unpainted;
        downcast<WebKit::DrawingAreaProxyCoordinatedGraphics>(*drawingArea).paint(canvas, paintRect, unpainted);
        for (const auto& rect : unpainted.rects())
            paintBackground(canvas, backgroundColor, rect);
    } else
        paintBackground(canvas, backgroundColor, paintRect);
}

static OptionSet<WebKit::WebEventModifier> modifiersOf(unsigned bits)
{
    OptionSet<WebKit::WebEventModifier> out;
    if (bits & kWKKosmosModifierShift)
        out.add(WebKit::WebEventModifier::ShiftKey);
    if (bits & kWKKosmosModifierControl)
        out.add(WebKit::WebEventModifier::ControlKey);
    if (bits & kWKKosmosModifierAlt)
        out.add(WebKit::WebEventModifier::AltKey);
    if (bits & kWKKosmosModifierMeta)
        out.add(WebKit::WebEventModifier::MetaKey);
    return out;
}

void WKPageMouseKosmos(WKPageRef pageRef, int type, int button, unsigned buttons, int x, int y, int clicks, unsigned modifiers)
{
    auto eventType = type == kWKKosmosMouseDown ? WebKit::WebEventType::MouseDown
        : type == kWKKosmosMouseUp ? WebKit::WebEventType::MouseUp
        : WebKit::WebEventType::MouseMove;
    auto which = button == 0 ? WebKit::WebMouseEventButton::Left
        : button == 1 ? WebKit::WebMouseEventButton::Middle
        : button == 2 ? WebKit::WebMouseEventButton::Right
        : WebKit::WebMouseEventButton::None;
    WebCore::DoublePoint position(x, y);

    WebKit::WebMouseEvent event({ eventType, modifiersOf(modifiers), MonotonicTime::now() }, which,
        static_cast<unsigned short>(buttons), position, position, 0, 0, 0,
        eventType == WebKit::WebEventType::MouseMove ? 0 : clicks, 0, WebKit::WebEventInputSource::UserDriven);
    WebKit::toImpl(pageRef)->handleMouseEvent(WebKit::NativeWebMouseEvent(event));
}

void WKPageWheelKosmos(WKPageRef pageRef, int x, int y, float dx, float dy, float ticksX, float ticksY, unsigned modifiers)
{
    WebCore::IntPoint position(x, y);

    WebKit::WebWheelEvent event({ WebKit::WebEventType::Wheel, modifiersOf(modifiers), MonotonicTime::now() },
        position, position, WebCore::FloatSize(dx, dy), WebCore::FloatSize(ticksX, ticksY),
        WebKit::WebWheelEvent::Granularity::ScrollByPixelWheelEvent);
    WebKit::toImpl(pageRef)->handleNativeWheelEvent(WebKit::NativeWebWheelEvent(event));
}

void WKPageKeyKosmos(WKPageRef pageRef, bool down, const char* key, const char* code, const char* text, int virtualKey, unsigned modifiers)
{
    auto keyName = String::fromUTF8(key ? key : "Unidentified");
    auto codeName = String::fromUTF8(code ? code : "Unidentified");
    auto typed = down && text ? String::fromUTF8(text) : String();

    // WebCore's old name for a key: "U+0041" for a character, the DOM's
    // name otherwise, which for the keys WebCore asks about is the same.
    String identifier = keyName;
    if (keyName.length() == 1)
        identifier = makeString("U+"_s, hex(keyName.convertToASCIIUppercase()[0], 4));

    WebKit::WebKeyboardEvent event({ down ? WebKit::WebEventType::KeyDown : WebKit::WebEventType::KeyUp, modifiersOf(modifiers), MonotonicTime::now() },
        typed, typed, keyName, codeName, identifier, virtualKey, virtualKey, 0, false, false, false);
    WebKit::toImpl(pageRef)->handleKeyboardEvent(WebKit::NativeWebKeyboardEvent(event));
}

// A string into a fixed field, cut to fit and always ended.
static void copyInto(const String& string, char* out, size_t room)
{
    auto utf8 = string.utf8();
    size_t length = std::min<size_t>(utf8.length(), room - 1);
    memcpy(out, utf8.data(), length);
    out[length] = '\0';
}

// Who signed a certificate, as its own issuer field says: the common name,
// else the organisation, else the whole name. A server sends its own
// certificate and often nothing above it, so the field is the one place
// this is always said.
static void issuerOf(const Vector<uint8_t>& pem, char* out, size_t room)
{
    out[0] = '\0';
    BIO* bio = BIO_new_mem_buf(pem.span().data(), static_cast<int>(pem.size()));
    if (!bio)
        return;
    X509* certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!certificate)
        return;
    const X509_NAME* name = X509_get_issuer_name(certificate);
    bool said = false;
    for (int nid : { NID_commonName, NID_organizationName }) {
        int index = X509_NAME_get_index_by_NID(name, nid, -1);
        const ASN1_STRING* data = index < 0 ? nullptr : X509_NAME_ENTRY_get_data(X509_NAME_get_entry(name, index));
        unsigned char* utf8 = nullptr;
        int length = data ? ASN1_STRING_to_UTF8(&utf8, data) : -1;
        if (length > 0) {
            size_t n = std::min<size_t>(length, room - 1);
            memcpy(out, utf8, n);
            out[n] = '\0';
            said = true;
        }
        OPENSSL_free(utf8);
        if (said)
            break;
    }
    if (!said)
        X509_NAME_oneline(name, out, static_cast<int>(room));
    X509_free(certificate);
}

void WKPageGetSecurityKosmos(WKPageRef pageRef, WKSecurityKosmos* out)
{
    memset(out, 0, sizeof *out);
    RefPtr page = WebKit::toImpl(pageRef);
    auto& state = page->pageLoadState();

    out->secureOnly = state.hasOnlySecureContent();
    out->insecureContent = state.committedHasInsecureContent();

    auto& info = state.certificateInfo();
    auto& chain = info.certificateChain();
    out->verificationError = info.verificationError();
    out->chainLength = chain.size();
    if (chain.isEmpty())
        return;

    if (auto summary = info.summary()) {
        copyInto(summary->subject, out->subject, sizeof out->subject);
        out->validFrom = summary->validFrom.seconds();
        out->validUntil = summary->validUntil.seconds();
        // Its DNS names, then its addresses: a server named by an address
        // is held to the second (`run_tls.pki`).
        String names;
        for (const auto* list : { &summary->dnsNames, &summary->ipAddresses }) {
            for (const String& name : *list)
                names = names.isEmpty() ? name : makeString(names, ", "_s, name);
        }
        copyInto(names, out->names, sizeof out->names);
    }

    issuerOf(chain[0], out->issuer, sizeof out->issuer);
}

void WKAuthenticationDecisionListenerPerformDefaultHandlingKosmos(WKAuthenticationDecisionListenerRef listener)
{
    protect(WebKit::toImpl(listener))->completeChallenge(WebKit::AuthenticationChallengeDisposition::PerformDefaultHandling);
}

void WKKosmosSetFramesTraced(bool on)
{
    WebKit::kosmosFramesTraced = on;
}

void WKKosmosOnMessages(void (*told)(void))
{
    WebKit::kosmosOnMessages(told);
}

unsigned WKKosmosTakeIncorporated(double* average, double* most, double* fromRing, double* fromRingMost, double* scrolled, double* scrolledMost)
{
    return WebKit::kosmosTakeIncorporated(average, most, fromRing, fromRingMost, scrolled, scrolledMost);
}
