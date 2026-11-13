/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **Accessibility on Kosmos, with no service yet** (`docs/w6.md` W6b;
 * `0017-accessibility-with-no-service.patch`). WebCore builds its tree of
 * accessibility objects on every port and tells the platform when it
 * changes; here each object gets the empty wrapper PlayStation's and
 * Haiku's ports give theirs, and what the platform is told goes nowhere -
 * as nothing in Kosmos listens yet. When Astra has an accessibility
 * service, these are where it is told.
 */

#include "config.h"
#include "AXObjectCache.h"

#include "AccessibilityObject.h"

namespace WebCore {

void AXObjectCache::attachWrapper(AccessibilityObject& object)
{
    auto wrapper = adoptRef(*new AccessibilityObjectWrapper());
    object.setWrapper(wrapper.ptr());
}

void AXObjectCache::detachWrapper(AXCoreObject*, AccessibilityDetachmentType)
{
}

void AXObjectCache::postPlatformNotification(AccessibilityObject&, AXNotification)
{
}

void AXObjectCache::nodeTextChangePlatformNotification(AccessibilityObject*, AXTextChange, unsigned, const String&)
{
}

void AXObjectCache::frameLoadingEventPlatformNotification(RenderView*, AXLoadingEvent)
{
}

void AXObjectCache::handleScrolledToAnchor(const Node&)
{
}

void AXObjectCache::platformHandleFocusedUIElementChanged(AccessibilityObject*, AccessibilityObject*)
{
}

void AXObjectCache::platformPerformDeferredCacheUpdate()
{
}

void AccessibilityObject::detachPlatformWrapper(AccessibilityDetachmentType)
{
}

bool AccessibilityObject::accessibilityIgnoreAttachment() const
{
    return false;
}

AccessibilityObjectInclusion AccessibilityObject::accessibilityPlatformIncludesObject() const
{
    return AccessibilityObjectInclusion::DefaultBehavior;
}

} // namespace WebCore
