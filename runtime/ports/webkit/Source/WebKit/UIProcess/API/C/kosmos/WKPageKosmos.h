/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A page's picture, into a surface** (`docs/w7.md` W7a): the last
 * picture the web process sent, painted into pixels a caller holds - a
 * window's surface in JetSki, a region in the suite. Kosmos's pixels have
 * a pitch, which is almost never the width times four, so it is said.
 * Where the page has sent nothing yet, its background colour, or white.
 *
 * **And what a person does to it** (W7c): the pointer, the wheel and keys,
 * made into WebKit's own events here - WebKit has no public call for any
 * of them, each port building them from its toolkit's - and handed to the
 * page as GTK's and WPE's views hand theirs. Positions are in the view's
 * pixels; modifiers are the `WKKosmosModifier` bits.
 */

#pragma once

#include <WebKit/WKBase.h>
#include <WebKit/WKGeometry.h>
#include <WebKit/WKView.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

WK_EXPORT void WKPagePaintKosmos(WKPageRef, uint32_t* pixels, unsigned width, unsigned height, unsigned pitch, WKRect paintRect);

enum {
    kWKKosmosModifierShift = 1 << 0,
    kWKKosmosModifierControl = 1 << 1,
    kWKKosmosModifierAlt = 1 << 2,
    kWKKosmosModifierMeta = 1 << 3,
};

enum {
    kWKKosmosMouseDown,
    kWKKosmosMouseUp,
    kWKKosmosMouseMove,
};

/* `button`: 0 the left, 1 the middle, 2 the right, -1 none (a move with
 * nothing held). `buttons` is what is held, as the DOM counts them: 1 the
 * left, 2 the right, 4 the middle. `clicks` is 1, 2 for a double click. */
WK_EXPORT void WKPageMouseKosmos(WKPageRef, int type, int button, unsigned buttons, int x, int y, int clicks, unsigned modifiers);

/* `dx`, `dy` in pixels, positive towards the start - up and left, as
 * WebKit counts a wheel; `ticks` the notches. */
WK_EXPORT void WKPageWheelKosmos(WKPageRef, int x, int y, float dx, float dy, float ticksX, float ticksY, unsigned modifiers);

/* One key going down or coming up. `key` and `code` are the DOM's names
 * ("a", "KeyA"; "Enter", "Enter"), `text` what it types, if anything, in
 * UTF-8, `virtualKey` Windows's code, which WebCore's editing commands
 * are keyed by everywhere but the Mac. */
WK_EXPORT void WKPageKeyKosmos(WKPageRef, bool down, const char* key, const char* code, const char* text, int virtualKey, unsigned modifiers);

/*
 * **Web Inspector, docked** (`docs/webkit.html`, *Developer mode*, agreed
 * 15 November 2026): WebKit makes the inspector's own page, in a view of
 * its own, and the application shows it beside the page it inspects. The
 * handler is told when an inspected page's inspector view appears - and,
 * with a null view, when it goes - so the window can split and join. The
 * front end's files are a folder on the disk (`Main.html` in it), read as
 * any page from the disk is.
 */
typedef void (*WKInspectorViewHandlerKosmos)(WKPageRef inspected, WKViewRef inspector, void* context);
WK_EXPORT void WKInspectorSetViewHandlerKosmos(WKInspectorViewHandlerKosmos, void* context);
WK_EXPORT void WKInspectorSetResourcesKosmos(const char* folder);

/*
 * **How a page came** (`docs/padlock.html`, Diego, 16 November 2026: "its
 * lacking a ssl certificate and safe page indicator"): what the UI process
 * knows of the page committed - whether it came over HTTPS with nothing
 * over plain HTTP in it, and the certificate WebKit's network process saw,
 * as curl handed it on. The issuer is what the certificate's own issuer
 * field names. Times are seconds since 1970; names are the certificate's
 * DNS names, with ", " between them, as many as fit.
 */
struct WKSecurityKosmos {
    bool secureOnly;                /* HTTPS, and nothing came over plain HTTP */
    bool insecureContent;           /* something it showed or ran came plain */
    int verificationError;          /* OpenSSL's, as curl saw it; 0 checked out */
    unsigned chainLength;           /* certificates the server sent; 0 none known */
    char subject[256];
    char issuer[256];
    char names[512];
    double validFrom, validUntil;
};
WK_EXPORT void WKPageGetSecurityKosmos(WKPageRef, struct WKSecurityKosmos*);

/*
 * **A challenge left to WebKit**, as no client at all would: the C API
 * offers a credential, a cancel and a rejection, and not this - which is
 * what a page owner that answers only some challenges (a certificate
 * opened anyway) needs for the rest.
 */
WK_EXPORT void WKAuthenticationDecisionListenerPerformDefaultHandlingKosmos(WKAuthenticationDecisionListenerRef);

/*
 * **A frame's time, traced** (`KosmosFrameTimes.h`): on before a page's
 * process is started, it is timed too; and the updates the UI process took
 * in since the last asking - how many, on average and the most, in ms.
 */
WK_EXPORT void WKKosmosSetFramesTraced(bool);

/*
 * **Told when a connection has news** (`KosmosWake.h`): `told` is called
 * on a receiving thread after messages arrive, and must not block.
 */
WK_EXPORT void WKKosmosOnMessages(void (*told)(void));
WK_EXPORT unsigned WKKosmosTakeIncorporated(double* average, double* most, double* fromRing, double* fromRingMost, double* scrolled, double* scrolledMost);

/*
 * **The page shown as it is, never copied** (`webkit.md` *Fast* 1c): the
 * UI process's backing store made in memory the caller makes -
 * `WKKosmosSetStoreMemory`, a region a window manager can be handed - as
 * two buffers, written one while the other is shown. `WKPageShowKosmos`
 * says the buffer just finished, which the caller shows from now, and
 * what changed in it, and starts writing the other; false when nothing
 * changed, or when this page's store is not in such memory (painted into
 * the window by `WKPagePaintKosmos` then, as before) - unless `everything`,
 * which says the whole buffer changed, for a store not shown before. A new `generation`
 * is a new store - another size, another page - and a new region.
 */
struct WKKosmosShown {
    long region;
    unsigned width, height, pitch;
    unsigned long bytes;            /* each buffer's; the second follows the first */
    int buffer;                     /* 1 or 2 */
    int x, y, w, h;                 /* what changed, in the buffer's pixels */
    unsigned generation;
};
WK_EXPORT bool WKPageShowKosmos(WKPageRef, bool everything, struct WKKosmosShown*);
WK_EXPORT void WKKosmosSetStoreMemory(bool (*make)(unsigned long bytes, void** at, long* region),
    void (*release)(void* at, unsigned long bytes, long region));
/* For a suite's control only (`KosmosStore.h`): the other buffer owes
 * nothing, and shows what it held before. */
WK_EXPORT void WKKosmosSetStoreForgets(bool);

/*
 * **Find on the page, with which match is shown** (`docs/webkit.html`,
 * *Find on the page*). The C API's find client says how many matches there
 * are and not which one is selected; WebKit's own client is told both,
 * when it is asked with `DetermineMatchIndex`, which the C options cannot
 * say. `found` hears `count` (0 when there is none, `(unsigned)-1` past
 * `most`) and the shown match's `index` from 0. Every match is marked, the
 * one found selected and scrolled to; WKPageHideFindUI takes the marks off.
 */
typedef void (*WKPageFoundKosmos)(WKPageRef, unsigned count, int index, bool wrapped, void* context);
WK_EXPORT void WKPageSetFindHandlerKosmos(WKPageRef, WKPageFoundKosmos, void* context);
WK_EXPORT void WKPageFindKosmos(WKPageRef, const char* words, bool matchCase, bool backwards, unsigned most);

#ifdef __cplusplus
}
#endif
