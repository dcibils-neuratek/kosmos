/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The hook `KosmosWake.h` describes: one function, set once by whoever
 * runs the main loop in slices, read by every receiving thread.
 */

#include "config.h"
#include "KosmosWake.h"

#include <atomic>

namespace WebKit {

static std::atomic<void (*)()> told { nullptr };

void kosmosOnMessages(void (*function)())
{
    told.store(function, std::memory_order_release);
}

void kosmosMessagesArrived()
{
    if (auto function = told.load(std::memory_order_acquire))
        function();
}

} // namespace WebKit
