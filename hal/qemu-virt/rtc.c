/*
 * PL031 real-time clock on QEMU virt.
 *
 * Base address: read out of the device tree this QEMU actually publishes,
 * rather than remembered - `-M virt,dumpdtb=` names the node `pl031@9010000`
 * with `compatible = "arm,pl031"`. That is the machine answering for itself,
 * which is what `CLAUDE.md` asks for when an address is at stake.
 *
 * Register offsets: ARM PrimeCell Real Time Clock (PL031) Technical
 * Reference Manual, ARM DDI 0224B, chapter 3. Only one register is read.
 *
 * The whole driver is one load, and that is not an accident of this machine:
 * a PL031 counts seconds in a 32-bit register and QEMU starts it from the
 * host clock, so there is no initialisation, no interrupt, no state and
 * nothing to get wrong except the address. Everything a date is made of -
 * months, leap years, the fact that a day is not always 86400 seconds long
 * somewhere else - happens in Lua, where it can be read and fixed.
 */

#include <stdint.h>

#include "mmio.h"
#include "qemu-virt.h"
#include "hal.h"

#define RTC_BASE        0x09010000UL

#define RTC_DR          (RTC_BASE + 0x00)   /* data: seconds, read-only */

/*
 * Seconds since 1970, or zero when there is no clock.
 *
 * Zero as "no clock" rather than a separate `bool` out-parameter, because
 * zero is 1 January 1970 and no machine is being started then. The caller
 * gets one number and one thing to check, and a board with no RTC returns
 * the same zero as a board whose RTC has never been set - which is the same
 * fact for anyone who wanted to print a date.
 */
unsigned long hal_rtc_seconds(void)
{
    return (unsigned long)mmio_read32(RTC_DR);
}
