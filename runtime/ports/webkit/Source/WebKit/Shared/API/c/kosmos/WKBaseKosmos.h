/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The one type of WebKit2's C API that is a platform's own: the view a
 * page is shown in (`docs/w7.md` W7a). It is PlayStation's view, which
 * without a WPE backend is nothing of PlayStation's.
 */

#ifndef WKBaseKosmos_h
#define WKBaseKosmos_h

#ifndef WKBase_h
#error "Please #include \"WKBase.h\" instead of this file directly."
#endif

typedef const struct OpaqueWKView* WKViewRef;

#endif /* WKBaseKosmos_h */
