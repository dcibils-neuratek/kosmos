/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * ACPI mode, the power button, and the embedded controller's events - the
 * keys a laptop's firmware reports rather than its keyboard.
 *
 * A ThinkPad's F5 and F6 are not keys (`docs/thinkpad.md` 8b): the embedded
 * controller takes them and raises an event, and the system asks it which -
 * 14h brighter, 15h dimmer. **Until a system switches the machine into ACPI
 * mode, the firmware's own hidden handler (SMM) services those events
 * itself**, and on the T14 it did so faster than a four-millisecond watch
 * could see: stick 0.10.85 read `SCI_EN is clear`, found the controller
 * where its ECDT says, and saw not one change in its status or in any GPE
 * status bit while F5 and F6 were pressed (`testing.md` 18.99). So, with
 * Diego's yes on 19 September:
 *
 *   at boot   the switch every system makes - the FADT's "ACPI enable"
 *             value written to its SMI command port, and SCI_EN waited for;
 *   each tick on core 0, the power button's status in PM1a, and the
 *             controller's SCI_EVT - when it is set, the controller is asked
 *             which event with QR_EC, and the answer becomes a key.
 *
 * **Watched, not interrupted.** The SCI is never unmasked: a level-triggered
 * line nothing here would clear is a line that fires for ever. A tick is
 * four milliseconds, which a key press does not notice, and reading two
 * registers on one core is nothing.
 *
 * **What ACPI mode costs is named, and Diego agreed to it.** The power
 * button and the lid report to the system instead of the firmware. The
 * power button becomes `KEY_POWER`, which the window manager answers by
 * shutting down; holding it for four seconds still forces the machine off,
 * because that is the chipset's, not the firmware's. The lid does nothing,
 * as it did. The fans and the temperature stay the embedded controller's,
 * as they were, and the processor still throttles itself.
 *
 * **Every other query is answered too**, and said once: a controller whose
 * events nobody collects keeps them queued. Which number means brighter is
 * Lenovo's, read from the T14's own DSDT (`_Q14`, `_Q15`); another maker's
 * controller would name its own, and they would be logged here as unknown
 * until somebody reads that machine's DSDT the same way.
 *
 * Offsets and bits are the ACPI specification's and ACPICA's: the FADT and
 * ECDT fields in `acpi.c`, from `iasl -T` templates compiled and
 * disassembled; the controller's status bits OBF 0, IBF 1, SCI_EVT 5 and its
 * query command 84h; PM1 control's SCI_EN, bit 0; PM1 status's PWRBTN_STS,
 * bit 8, cleared by writing a one to it.
 */

#include <stdbool.h>
#include <stdint.h>

#include "acpi.h"
#include "console.h"
#include "ec.h"
#include "hal.h"
#include "keys.h"
#include "multiboot.h"
#include "pc.h"
#include "spinlock.h"

#define EC_OBF          0x01u
#define EC_IBF          0x02u
#define EC_SCI_EVT      0x20u
#define EC_QR_EC        0x84u

#define PM1_SCI_EN      0x0001u
#define PM1_PWRBTN      0x0100u

#define EC_DATA_DEFAULT 0x62u
#define EC_CMD_DEFAULT  0x66u

/*
 * How long a transaction may wait for the controller, in reads of its status
 * port - each about a microsecond on the LPC bus. A controller answers in
 * tens of them; one that does not within two thousand is said to be silent
 * and the event is left for the next tick, rather than holding a core with
 * its interrupts off.
 */
#define EC_SPINS        2000u

/* Queries asked in one tick at most: a controller with a queue of events
 * empties it over a few ticks rather than in one long one. */
#define QUERIES_A_TICK  4u

/* Lines about events said at most, as the watch before this had. */
#define EVENTS_SAID     64u

static bool     acpi_mode;
static uint16_t pm1_status;
static bool     ec_on;
static uint16_t ec_cmd;
static uint16_t ec_data;
static uint16_t gpe_status;     /* the byte holding the controller's GPE */
static uint8_t  gpe_bit;
static unsigned said;
static uint32_t unknown_said[8];    /* a bit per query number, said once */

/*
 * The keys, queued for `hal_key_event`. Written on core 0's tick and read by
 * a syscall on any core, so under a lock - which masks interrupts, as every
 * lock here does.
 */
#define ECQ 16

static struct spinlock ec_lock = SPINLOCK("ec keys");
static struct { uint8_t code; uint8_t down; } keys[ECQ];
static unsigned key_head, key_tail;

static void queue_key(unsigned code)
{
    unsigned long flags = spin_lock(&ec_lock);
    unsigned d;

    for (d = 0; d < 2; d++) {           /* a press and its release */
        unsigned next = (key_head + 1) % ECQ;

        if (next == key_tail) {
            key_tail = (key_tail + 1) % ECQ;    /* full: drop the oldest */
        }

        keys[key_head].code = (uint8_t)code;
        keys[key_head].down = (uint8_t)(d == 0);
        key_head = next;
    }

    spin_unlock(&ec_lock, flags);
}

bool ec_key_event(unsigned *code, bool *down)
{
    unsigned long flags = spin_lock(&ec_lock);
    bool got = key_head != key_tail;

    if (got) {
        *code = keys[key_tail].code;
        *down = keys[key_tail].down != 0;
        key_tail = (key_tail + 1) % ECQ;
    }

    spin_unlock(&ec_lock, flags);
    return got;
}

bool ec_input_pending(void)
{
    return key_head != key_tail;        /* a hint; the lock decides above */
}

static void put_hex8(unsigned v)
{
    kputx(v, 2);
}

/*
 * The switch every system makes. True when events now come to the system:
 * because SCI_EN was already set, because this set it, or because the
 * machine is hardware-reduced and has no other mode.
 */
static bool switch_to_acpi(const struct acpi_ec_facts *f)
{
    unsigned waited;

    if (f->hardware_reduced) {
        kputs("acpi: hardware reduced - events are the system's already\n");
        return true;
    }

    if (f->pm1a_cnt == 0) {
        kputs("acpi: no PM1a control block, so no ACPI mode to be in\n");
        return false;
    }

    if ((pc_in16((uint16_t)f->pm1a_cnt) & PM1_SCI_EN) != 0) {
        kputs("acpi: SCI_EN was already set - ACPI mode\n");
        return true;
    }

    if (f->smi_cmd == 0 || f->acpi_enable == 0) {
        kputs("acpi: SCI_EN is clear and the FADT gives no way to set it\n");
        return false;
    }

    pc_out8((uint16_t)f->smi_cmd, (uint8_t)f->acpi_enable);

    /* The firmware sets it before the write returns, almost always; three
     * seconds is what the specification's own examples allow. */
    for (waited = 0; waited < 300; waited++) {
        if ((pc_in16((uint16_t)f->pm1a_cnt) & PM1_SCI_EN) != 0) {
            kputs("acpi: switched to ACPI mode - 0x");
            put_hex8(f->acpi_enable);
            kputs(" to port 0x");
            kputx(f->smi_cmd, 2);
            kputs(", SCI_EN set after ");
            kputu(waited * 10u);
            kputs(" ms\n");
            return true;
        }

        pc_timer_wait_ms(10);
    }

    kputs("acpi: 0x");
    put_hex8(f->acpi_enable);
    kputs(" written to port 0x");
    kputx(f->smi_cmd, 2);
    kputs(" and SCI_EN never set - the firmware kept its events\n");
    return false;
}

void ec_init(void)
{
    struct acpi_ec_facts f;
    uint8_t status;

    if (!acpi_ec_facts(&f)) {
        kputs("acpi: no FADT, so nothing to say about events\n");
        return;
    }

    kputs("acpi: the FADT: SCI ");
    kputu(f.sci_int);
    kputs(", SMI command port 0x");
    kputx(f.smi_cmd, 2);
    kputs(" (0x");
    put_hex8(f.acpi_enable);
    kputs(" enables ACPI), PM1a event at 0x");
    kputx(f.pm1a_evt, 4);
    kputs(", PM1a control at 0x");
    kputx(f.pm1a_cnt, 4);
    kputs(", GPE0 at 0x");
    kputx(f.gpe0_blk, 4);
    kputs(", ");
    kputu(f.gpe0_len);
    kputs(" bytes");
    kputs(f.hardware_reduced ? ", hardware reduced\n" : "\n");

    acpi_mode = switch_to_acpi(&f);

    if (!acpi_mode) {
        return;
    }

    /*
     * The power button's status, cleared of any press from before now, and
     * its enable set. Intel's chipsets set the status whatever the enable
     * says; QEMU's sets it only when enabled, and an enabled button with
     * SCI_EN set asserts the SCI - which is masked at the interrupt
     * controller, as every line nothing claimed is, so the tick is what
     * hears it either way.
     */
    if (f.pm1a_evt != 0 && f.pm1_evt_len >= 4) {
        uint16_t enable = (uint16_t)(f.pm1a_evt + f.pm1_evt_len / 2u);

        pm1_status = (uint16_t)f.pm1a_evt;
        pc_out16(pm1_status, PM1_PWRBTN);
        pc_out16(enable, (uint16_t)(pc_in16(enable) | PM1_PWRBTN));
        kputs("acpi: the power button is a key now - KEY_POWER\n");
    }

    ec_cmd  = f.ec_cmd  != 0 ? (uint16_t)f.ec_cmd  : EC_CMD_DEFAULT;
    ec_data = f.ec_data != 0 ? (uint16_t)f.ec_data : EC_DATA_DEFAULT;

    kputs(f.ec_cmd != 0 ? "ec: the ECDT puts the controller at 0x"
                        : "ec: no ECDT; the controller looked for at 0x");
    kputx(ec_cmd, 2);

    status = pc_in8(ec_cmd);

    if (status == 0xffu) {
        kputs(" - nothing answers there\n");
        return;
    }

    kputs(" and 0x");
    kputx(ec_data, 2);
    kputs(", status 0x");
    put_hex8(status);
    kputs(" - its events are asked for from now on\n");

    /* The controller's GPE, from the ECDT, to clear once its events are
     * collected - tidiness rather than need, since the SCI stays masked. */
    if (f.ec_cmd != 0 && f.gpe0_blk != 0 && f.ec_gpe / 8u < f.gpe0_len / 2u) {
        gpe_status = (uint16_t)(f.gpe0_blk + f.ec_gpe / 8u);
        gpe_bit = (uint8_t)(1u << (f.ec_gpe % 8u));
    }

    ec_on = true;
}

/* The status port until `mask` reads as `want`, or -1 once EC_SPINS pass. */
static int ec_wait(uint8_t mask, uint8_t want)
{
    unsigned i;

    for (i = 0; i < EC_SPINS; i++) {
        uint8_t s = pc_in8(ec_cmd);

        if ((s & mask) == want) {
            return s;
        }
    }

    return -1;
}

/* QR_EC: which event the controller has. 0 when none; -1 when it did not
 * answer in time. */
static int ec_query(void)
{
    if (ec_wait(EC_IBF, 0) < 0) {
        return -1;
    }

    pc_out8(ec_cmd, EC_QR_EC);

    if (ec_wait(EC_OBF, EC_OBF) < 0) {
        return -1;
    }

    return pc_in8(ec_data);
}

static void event(int q)
{
    unsigned code = 0;

    if (q == 0x14) {
        code = KEY_BRIGHTNESSUP;
    } else if (q == 0x15) {
        code = KEY_BRIGHTNESSDOWN;
    }

    if (code != 0) {
        queue_key(code);
    }

    if (said < EVENTS_SAID
        && (code != 0 || (unknown_said[q >> 5] & (1u << (q & 31))) == 0)) {
        kputs("ec: query 0x");
        put_hex8((unsigned)q);
        kputs(q == 0x14 ? " - brightness up\n"
              : q == 0x15 ? " - brightness down\n"
              : " - nothing is bound to it, said once\n");

        if (code == 0) {
            unknown_said[q >> 5] |= 1u << (q & 31);
        }

        said++;
    }
}

void ec_tick(void)
{
    unsigned n;

    if (!acpi_mode) {
        return;
    }

    if (pm1_status != 0 && (pc_in16(pm1_status) & PM1_PWRBTN) != 0) {
        pc_out16(pm1_status, PM1_PWRBTN);       /* write one to clear */
        queue_key(KEY_POWER);
        kputs("acpi: the power button\n");
    }

    if (!ec_on) {
        return;
    }

    for (n = 0; n < QUERIES_A_TICK; n++) {
        uint8_t s = pc_in8(ec_cmd);
        int q;

        if (s == 0xffu || (s & EC_SCI_EVT) == 0) {
            break;
        }

        q = ec_query();

        if (q < 0) {
            if (said < EVENTS_SAID) {
                kputs("ec: the controller did not answer a query in time\n");
                said++;
            }

            break;
        }

        if (q == 0) {
            break;
        }

        event(q);
    }

    if (gpe_status != 0 && (pc_in8(gpe_status) & gpe_bit) != 0) {
        pc_out8(gpe_status, gpe_bit);           /* write one to clear */
    }
}
