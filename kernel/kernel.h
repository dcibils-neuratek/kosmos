#ifndef KERNEL_KERNEL_H
#define KERNEL_KERNEL_H

/*
 * The scheduler tick rate.
 *
 * **250 Hz, and audio is what decided it.** It was 100 for a long time,
 * because 100 is what every piece of reference material assumes and a 10 ms
 * period is comfortably longer than an interrupt costs even under QEMU.
 * Nothing in the system had an opinion, so nothing challenged it.
 *
 * The sound device has an opinion. It consumes a period every 5.8 ms, and a
 * server that services it from a timer cannot hold up a 5.8 ms pipeline by
 * waking every 10 ms - it is not a matter of buffering more, which was
 * tried: doubling the queue to 46 ms changed nothing, because the shortfall
 * is in how often the queue is *topped up*, not how much it holds.
 *
 * Measured, delivering 580 ms of audio: 502 ms with the server spinning,
 * 922 ms with it sleeping at 100 Hz, 520 ms with it sleeping at 250 Hz.
 * The first and the last sound the same and cost 89 points of processor
 * apart, which is the whole argument.
 *
 * A tick is now 4 ms. That is 250 timer interrupts a second against 100 -
 * measurably nothing next to what it replaced - and it is *also* the right
 * direction for a system whose stated goal is that input reaches what draws
 * without waiting: the preemption granularity is what a missed deadline is
 * rounded up to, and it just got two and a half times finer.
 *
 * **Anything measured in ticks changed meaning here.** `sysinfo.tick_hz`
 * exists so that nothing has to hardcode the number, and the window
 * manager's input timeout now derives from it rather than being the literal
 * `1` it was when a tick happened to be the interval that program wanted.
 */
#define TICK_HZ     250

#endif /* KERNEL_KERNEL_H */
