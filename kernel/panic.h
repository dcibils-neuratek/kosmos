#ifndef KERNEL_PANIC_H
#define KERNEL_PANIC_H

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

#endif /* KERNEL_PANIC_H */
