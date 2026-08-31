#ifndef KERNEL_SYSCALL_H
#define KERNEL_SYSCALL_H


/*
 * The syscall interface.
 *
 * `svc #0`, with the number in x8 and arguments in x0 through x5, and the
 * result back in x0. That is the AArch64 Linux convention, borrowed because
 * it is the one every tool and every reader already knows; nothing else about
 * it is inherited.
 *
 * x8 rather than x0 for the number so the arguments start where the C calling
 * convention already puts them, which means a syscall stub is a `mov` and an
 * `svc` rather than a shuffle.
 *
 * SYS_TICKS is the one that looks out of place, and is not. `design.md` §4.4
 * makes the *clock* a capability - `/dev/clock`, asked for by name, handed
 * over or not - and that stays true: what a program wants is a date, and a
 * date comes from a server. But the server has to read the counter from
 * somewhere, and a monotonic tick is the kind of thing that genuinely cannot
 * be a message: it has to be sampled where the code being timed runs, or the
 * sample measures the sampling. So the raw counter is a syscall and the wall
 * clock stays a capability, which is the same split the design already makes
 * between entering the kernel and everything else.
 *
 * It also retires a weakness. Without it a process cannot read CNTPCT_EL0 at
 * all, and Lua's string-hash seed at EL0 came off a stack address for want of
 * anything better.
 *
 * These are not the interface Kosmos ends up with. `design.md` §4.4 has
 * processes reaching resources by name through a namespace, and at M5 most of
 * what is here becomes `list`, `read` and `write` against a path. What
 * survives is what genuinely cannot be a message: entering and leaving the
 * kernel, and the capability operations that everything else is expressed in.
 */

#define SYS_EXIT        0   /* (code)                    never returns */
#define SYS_WRITE       1   /* (ptr, len)             -> bytes written */
#define SYS_YIELD       2   /* ()                                      */
#define SYS_ENDPOINT    3   /* ()                     -> cap or error  */
#define SYS_CALL        4   /* (cap, msg, reply)      -> 0 or error    */
#define SYS_RECEIVE     5   /* (cap, msg, &sender)    -> 0 or error    */
#define SYS_REPLY       6   /* (sender, msg)          -> 0 or error    */
#define SYS_GETCHAR     7   /* ()                     -> byte, or -1    */
#define SYS_SPAWN       8   /* (arg, caps, ncaps, flags) -> child id     */
#define SYS_WAIT        9   /* (&id)                  -> exit code       */
#define SYS_TICKS      10   /* ()                     -> monotonic ticks  */
#define SYS_SCREEN     11   /* (&info)                -> 0 or error       */
#define SYS_SYSINFO    12   /* (&info)                -> 0 or error       */

#define SYS_MAX         13

/*
 * What a spawn may hand its child beyond capabilities.
 *
 * A flag rather than a capability, for the same reason `owns_console` is a
 * boolean: a device should be named by a capability the process holds, and
 * that needs a capability that names a device. Until then, whoever spawns
 * decides, which is at least the right shape - authority flows from parent
 * to child and never sideways.
 */
#define SPAWN_CONSOLE   1u
#define SPAWN_SCREEN    2u

/*
 * What SYS_SCREEN reports.
 *
 * The address is in the *caller's* address space, because the framebuffer is
 * mapped into a process that holds the screen and nowhere else. A process
 * without it never learns the number, which is the point: this is the whole
 * of what the kernel says about pixels, and it says it only to the one
 * process that was handed the device.
 *
 * The layout is written twice - here and in user/include/kosmos.h - for the
 * same reason `struct message` is, and is checked the same way.
 */
#ifndef __ASSEMBLER__
#include <stdint.h>

struct screen_info {
    uint64_t address;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;             /* bytes per row; never width * 4 */
    uint32_t reserved;
};

/*
 * What SYS_SYSINFO reports: the machine, and how much of it is in use.
 *
 * **The raw ID registers travel with the decoded numbers**, deliberately.
 * Decoding them is a table lookup and tables belong in Lua, not in the
 * kernel - `design.md` §1's whole argument is that policy goes up and
 * mechanism stays down. So the kernel reads registers and counts pools, and
 * userland decides what any of it means and how to say it.
 *
 * Not gated. A hardware inventory is not authority: it says nothing a
 * process could not learn by other means and grants nothing. It is the same
 * category as SYS_TICKS.
 *
 * The intended way to read this is `/dev`, through the namespace, the way
 * everything else is reached. This syscall is the door the device server
 * goes through, exactly as SYS_WRITE is the door the console server goes
 * through - and, unlike that one, nothing is lost by another process using
 * it directly.
 */
struct sysinfo {
    /* The processor, raw. arch/aarch64/cpu.c decodes the same values for
     * the boot log; userland decodes them again for /dev/cpu, because the
     * two want different amounts of detail and neither should constrain the
     * other. */
    uint64_t midr;
    uint64_t mpidr;
    uint64_t ctr;
    uint64_t pfr0;
    uint64_t isar0;
    uint64_t mmfr0;
    uint64_t counter_hz;

    /* Memory, in pages of PAGE_SIZE. */
    uint64_t ram_base;
    uint64_t ram_size;
    uint32_t pages_total;
    uint32_t pages_free;

    /* The fixed pools, and how full they are. Both halves matter: "3
     * processes" says nothing without "of 8". */
    uint32_t threads_used;
    uint32_t threads_total;
    uint32_t processes_used;
    uint32_t processes_total;
    uint32_t endpoints_used;
    uint32_t endpoints_total;
    uint32_t spaces_used;
    uint32_t spaces_total;

    /* Devices. Zero width means there is no display. */
    uint32_t screen_width;
    uint32_t screen_height;
    uint32_t screen_pitch;
    uint32_t has_keyboard;

    /* Ticks charged to the idle thread and to everything else, since boot.
     * Both only rise; a percentage is the difference between two readings,
     * which is the only kind that can mean "recently" rather than "ever". */
    uint64_t idle_ticks;
    uint64_t busy_ticks;

    uint32_t cpus;              /* cores the kernel is scheduling on */
    uint32_t tick_hz;
    uint32_t current_el;
    uint32_t page_size;
};
#endif

/* Errors are negative so `if (result < 0)` reads correctly on both sides. */
#define SYS_ERR_BADCALL   (-100)    /* no such syscall number */
#define SYS_ERR_FAULT     (-101)    /* a pointer the process may not touch */
#define SYS_ERR_DENIED    (-102)    /* this process does not hold the device */
#define SYS_NO_INPUT      (-103)    /* nothing waiting; not an error */
#define SYS_ERR_NO_CHILD  (-104)    /* nothing to wait for */
#define SYS_ERR_NO_ROOM   (-105)    /* out of processes, or out of memory */

/*
 * Everything above is plain preprocessor because user programs written in
 * assembly include this header for the numbers. Anything below would be a
 * stream of unknown mnemonics to the assembler.
 */
#ifndef __ASSEMBLER__

#include <stdint.h>

struct trapframe;

/* Called from the trap handler on an SVC from EL0. Writes the result back
 * into the frame, which is where the eret will take x0 from. */
void syscall_dispatch(struct trapframe *tf);

#endif /* !__ASSEMBLER__ */

#endif /* KERNEL_SYSCALL_H */
