// Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
//
// **WTF's signal handlers on a system with no signals** (`docs/w4.md`
// W4c). `wtf/threads/Signals.h` is how JavaScriptCore asks to hear of a
// fault - an access outside a WebAssembly memory, a trap a JIT planted -
// and to go on from it. Kosmos delivers no signals: a fault ends the
// process, and Nebula's report says where (`CLAUDE.md`, *Compatibility
// inside a process yes, at system level never*). So a handler can be
// added and the table keeps it, as the POSIX version does, and nothing
// ever calls it. With no JIT and no WebAssembly nothing here adds one;
// when the JIT's step comes, the faults it relies on are its question.
//
// Threads are stopped another way - by Nebula, `posix/ThreadingPOSIX.cpp`
// under `OS(KOSMOS)` - which is what the signal named `Usr` is for
// elsewhere.

#include "config.h"
#include <wtf/threads/Signals.h>

#if OS(KOSMOS)

#include <wtf/WTFConfig.h>

namespace WTF {

void SignalHandlers::add(Signal signal, SignalHandler&& handler)
{
    Config::AssertNotFrozenScope assertScope;

    ASSERT(signal < Signal::NumberOfSignals);
    RELEASE_ASSERT(initState == SignalHandlers::InitState::Initializing);

    size_t signalIndex = static_cast<size_t>(signal);
    size_t nextFree = numberOfHandlers[signalIndex];
    RELEASE_ASSERT(nextFree < maxNumberOfHandlers);
    new (&handlers[signalIndex][nextFree]) SignalHandler(WTF::move(handler));
    numberOfHandlers[signalIndex]++;
}

void activateSignalHandlersFor(Signal signal)
{
    RELEASE_ASSERT(g_wtfConfig.signalHandlers.initState >= SignalHandlers::InitState::Initializing);
    ASSERT_UNUSED(signal, signal < Signal::Unknown);
}

void addSignalHandler(Signal signal, SignalHandler&& handler)
{
    g_wtfConfig.signalHandlers.add(signal, WTF::move(handler));
}

void SignalHandlers::initialize()
{
    SignalHandlers& handlers = g_wtfConfig.signalHandlers;
    RELEASE_ASSERT(handlers.initState == SignalHandlers::InitState::Uninitialized);
    handlers.initState = SignalHandlers::InitState::Initializing;
}

void SignalHandlers::finalize()
{
    Config::AssertNotFrozenScope assertScope;
    SignalHandlers& handlers = g_wtfConfig.signalHandlers;
    RELEASE_ASSERT(handlers.initState == SignalHandlers::InitState::Initializing);
    handlers.initState = SignalHandlers::InitState::Finalized;
}

} // namespace WTF

#endif // OS(KOSMOS)
