/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A process told that its connections have news** (`testing.md`
 * 18.855). WebKit's messages are handled on the main thread's run loop,
 * and in the UI process that loop runs only while the WebKit Kit is
 * pumping it - between a window's polls. A picture that arrives in
 * between waited for the next poll to time out: 7 ms of a scrolled
 * frame's 14 on the M700. So each connection's receiving thread says,
 * after reading what was written to it, that messages have arrived; the
 * kit, given that, wakes its window's poll (a pulse to the window
 * manager) and the loop pumps at once.
 *
 * Nothing is said until something asks (`kosmosOnMessages`), so a web or
 * network process, whose main thread runs its loop for ever, never pays.
 */

#pragma once

namespace WebKit {

// Called on a connection's receiving thread, after records are read. It
// must not block: the receiving thread is what reads the next one.
void kosmosOnMessages(void (*told)());

// The receiving thread's half.
void kosmosMessagesArrived();

} // namespace WebKit
