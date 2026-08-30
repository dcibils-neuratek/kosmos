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

#endif /* KERNEL_KERNEL_H */
