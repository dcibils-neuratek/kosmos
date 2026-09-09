/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KERNEL_PANIC_H
#define KERNEL_PANIC_H

#include <stdbool.h>

/*
 * The kernel has reached a state it does not know how to continue from.
 *
 * Prints the message and halts. It never returns, and it is never a way to
 * report something a caller could have handled: if a caller can react, the
 * function returns an error instead.
 *
 * The line starts with the same "PANIC:" prefix the exception dump uses, so
 * the host test runner recognises both and stops the run immediately rather
 * than waiting out its timeout.
 */
void panic(const char *msg) __attribute__((noreturn));

/*
 * Whether the machine is on its way down.
 *
 * **What this is for is locks.** A panic prints, printing takes the console
 * lock, and the panic that matters most is the one raised *inside* a
 * console write - a fault while the lock is held. That deadlocks: the panic
 * waits ten million spins for a lock its own caller owns, gives up, panics
 * about the lock, and starts again. The machine says `spinlock: console
 * held by 0, wanted by 0` for ever and never says what actually happened.
 *
 * Seen twice in one afternoon, both times on the framebuffer path, both
 * times on a machine with no serial port - where a hang with no message is
 * the worst outcome there is.
 *
 * So from the first line of `panic` every lock stops being taken. Nothing
 * is racing with a machine that is halting, and a message on the panel is
 * worth more than a structure that stays consistent on the way to `hlt`.
 */
bool panicking(void);

#endif /* KERNEL_PANIC_H */
