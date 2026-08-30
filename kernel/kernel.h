#ifndef KERNEL_KERNEL_H
#define KERNEL_KERNEL_H

/*
 * The scheduler tick rate.
 *
 * 100 Hz because it is the rate every piece of reference material assumes
 * and it leaves a 10 ms period, which is comfortably longer than an
 * interrupt costs even under QEMU's emulation. It becomes a real decision
 * when there is a scheduler to serve, at M3.
 */
#define TICK_HZ     100

/*
 * How much memory Lua gets. `design.md` §5.2 asks for a bounded heap of
 * roughly 2 MB per server, for a reason that matters: a small heap collects
 * fast, and the GC pause is the number that decides whether the system
 * stutters. A large shared heap would make that pause unbounded.
 */
#define LUA_HEAP_PAGES  512     /* 2 MB at a 4 KB granule */

#endif /* KERNEL_KERNEL_H */
