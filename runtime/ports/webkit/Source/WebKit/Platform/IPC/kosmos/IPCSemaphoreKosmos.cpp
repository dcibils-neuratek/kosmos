/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **WebKit's semaphore between processes, on an endpoint** (`docs/w7.md`
 * W7a). A futex cannot cross a process, and an endpoint can: a signal is
 * a pulse, and a wait is a receive with a deadline. Nebula counts pulses
 * of one code rather than queueing them, so a wait that collects several
 * keeps the rest for the next waits (`m_surplus`). An endpoint goes with
 * the process that made it, so a wait whose signaller's process made the
 * semaphore and ended wakes with an error rather than waiting for ever.
 */

#include "config.h"
#include "IPCSemaphore.h"

#include <kosmos.h>

extern "C" unsigned long posix_ticks_for_ns(long long);

namespace IPC {

constexpr uint32_t pulseSignal = 1;

Semaphore::Semaphore()
    : m_endpoint(KosmosCapability::adopt(kosmos_endpoint()))
{
}

Semaphore::Semaphore(KosmosCapability&& endpoint)
    : m_endpoint(WTF::move(endpoint))
{
}

Semaphore::Semaphore(Semaphore&&) = default;
Semaphore& Semaphore::operator=(Semaphore&&) = default;

Semaphore::~Semaphore()
{
    destroy();
}

void Semaphore::signal()
{
    ASSERT_WITH_MESSAGE(!!m_endpoint, "Signalling on an invalid semaphore object");
    kosmos_pulse(m_endpoint.capability(), pulseSignal, 0);
}

// 0 ticks is no deadline at all, which is Nebula's word for for ever; a
// deadline already past is one tick, the least Nebula waits.
static bool waitOn(long endpoint, uint32_t& surplus, unsigned long ticks)
{
    if (surplus) {
        --surplus;
        return true;
    }
    while (true) {
        struct message received;
        uint64_t sender = 0;
        if (kosmos_receive(endpoint, &received, &sender, 0, ticks) < 0)
            return false;
        if (received.tag == KOSMOS_PULSE_TAG) {
            struct kosmos_pulse pulse;
            memcpy(&pulse, received.data, sizeof pulse);
            if (pulse.code != pulseSignal || !pulse.count)
                continue;
            surplus += pulse.count - 1;
            return true;
        }
        // Nothing calls a semaphore; one that did is answered and ignored.
        if (sender) {
            struct message answer { };
            kosmos_reply(sender, &answer);
        }
        if (received.cap_plus_one)
            kosmos_cap_drop(static_cast<long>(received.cap_plus_one) - 1);
    }
}

bool Semaphore::wait()
{
    return !!m_endpoint && waitOn(m_endpoint.capability(), m_surplus, 0);
}

bool Semaphore::waitFor(Timeout timeout)
{
    if (!m_endpoint)
        return false;
    if (timeout.isInfinity())
        return waitOn(m_endpoint.capability(), m_surplus, 0);
    long long ns = static_cast<long long>(timeout.secondsUntilDeadline().nanoseconds());
    unsigned long ticks = posix_ticks_for_ns(ns > 0 ? ns : 1);
    return waitOn(m_endpoint.capability(), m_surplus, ticks ? ticks : 1);
}

void Semaphore::destroy()
{
    m_endpoint = { };
}

} // namespace IPC
