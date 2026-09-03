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
#define SYS_RECEIVE     5   /* (cap, msg, &sender, flags) -> 0 or error */
#define SYS_REPLY       6   /* (sender, msg)          -> 0 or error    */
#define SYS_GETCHAR     7   /* ()                     -> byte, or -1    */
#define SYS_SPAWN       8   /* (arg, caps, ncaps, flags) -> child id     */
#define SYS_WAIT        9   /* (&id)                  -> exit code       */
#define SYS_TICKS      10   /* ()                     -> monotonic ticks  */
#define SYS_SCREEN     11   /* (&info)                -> 0 or error       */
#define SYS_SYSINFO    12   /* (&info)                -> 0 or error       */
#define SYS_MAP        13   /* (pages)                -> address or error  */
#define SYS_UNMAP      14   /* (address, pages)       -> 0 or error       */
#define SYS_SETNAME    15   /* (ptr, len)             -> 0 or error       */
#define SYS_PROCTABLE  16   /* (&entries, max)        -> count or error   */
#define SYS_ENDPOINT_DESTROY 17 /* (cap)              -> 0 or error       */
#define SYS_POINTER    18   /* (&state)               -> 0 or error       */
#define SYS_SCREEN_TAKE 19  /* (take)                 -> 0 or error       */
#define SYS_KILL       20   /* (id)                   -> 0 or error       */
#define SYS_WAIT_INPUT 21   /* (timer ticks)          -> 0                 */
#define SYS_LOG        22   /* (buffer, max)          -> bytes or error    */
#define SYS_MEM_CREATE 23   /* (pages)                -> cap or error       */
#define SYS_MEM_MAP    24   /* (cap)                  -> address or error   */
#define SYS_MEM_SIZE   25   /* (cap)                  -> pages or error     */

#define SYS_DISK_INFO  26   /* (out struct)           -> 0 or error         */
#define SYS_DISK_READ  27   /* (sector, buf, bytes)   -> bytes or error     */
#define SYS_DISK_WRITE 28   /* (sector, buf, bytes)   -> bytes or error     */

#define SYS_BOOT_OPT   29   /* (name, out, max)       -> length or error   */
#define SYS_CAP_DROP   30   /* (cap)                  -> 0 or error         */
#define SYS_SHARE_UNMAP 31  /* (address, pages)       -> 0 or error         */
#define SYS_SCHED_INFO 32   /* (&info)                -> 0 or error         */
#define SYS_SCHED_SET  33   /* (what, value)          -> 0 or error         */

/*
 * The next key transition: a keycode and whether it went down.
 *
 * Separate from SYS_GETCHAR rather than folded into it, because they are
 * different questions. A character is what a key *means* - shifted, mapped,
 * with an arrow spread over three bytes - and is what a terminal wants. A
 * transition is what the key *did*, and is what anything that cares about a
 * key being held has to have; no stream of characters can express it.
 *
 * Both come off the same pass through the device, so the two never
 * disagree about what happened.
 *
 * Gated on owning the console, exactly as SYS_GETCHAR is. That is not
 * ceremony: a process that can ask which keys are being pressed is a
 * keylogger, so this goes to the one process the kernel already trusts with
 * input, and everything else asks it.
 */
#define SYS_KEY_EVENT  34   /* (&code, &down)         -> 0, or SYS_NO_INPUT */

/*
 * Stop the machine, or start it again.
 *
 * Gated on `owns_procctl`, which is the same authority `SYS_KILL` uses to
 * end any process - and that is the right comparison rather than a
 * convenient one: turning the machine off ends every process on it, so the
 * right to do it is the right to end them all at once. Nothing new is
 * granted, and a program that may not kill a process it did not start
 * certainly may not do this.
 *
 * Does not return when it works.
 */
#define SYS_POWER      35   /* (0 off, 1 restart)     -> does not return   */

/*
 * One period of PCM, queued. 44100 Hz, stereo, signed sixteen-bit - the
 * format is `hal.h`'s and is not negotiable here.
 *
 * Returns 0 when it was taken and SYS_NO_INPUT when the queue is full,
 * which is not an error: a caller that is ahead of the device should be
 * told so rather than blocked, because blocking in an audio path is how a
 * frame gets missed somewhere else.
 */
#define SYS_SND_WRITE  36   /* (ptr, len)             -> 0, full, or error */

/*
 * How many periods the device has not finished with.
 *
 * The deadline, as a number. `roadmap.md` M11a promises a measurement
 * rather than a bound, and this is the measurement: at zero the device has
 * run dry and the next sound has a click in it.
 */
/*
 * The largest period any board here uses, so userland can size a buffer
 * without including the HAL. A number rather than the real one because the
 * real one is the board's: `sys.info().audio_period` says what this machine
 * actually uses, and this is only the ceiling.
 */
#define HAL_SND_PERIOD_BYTES_MAX 8192

/*
 * The rate every source is converted to, which userland needs in order to
 * do the converting. The board fixes it in `hal.h`; this is the ABI's copy
 * of the same number, and `sys.info().audio_rate` reports what the machine
 * actually has. They agree today because there is one board.
 */
#define HAL_SND_RATE 44100u

#define SYS_SND_QUEUED 37   /* ()                     -> periods in flight */

/*
 * Sleep for a number of scheduler ticks.
 *
 * `SYS_WAIT_INPUT` has done this since M6 and does it well, but only for
 * the one process that owns the console, because it also wakes on a key.
 * Everything else that wanted to wait had `SYS_YIELD`, which does not wait
 * at all: it goes to the back of its band and comes straight back, so a
 * thread "waiting" this way is runnable for ever and a core is gone.
 *
 * That was measured rather than reasoned about. Playing a tone put the
 * audio server at 26% and the program feeding it at 63%, against 8% for
 * Doom - which draws a 320x200 frame thirty-five times a second and is
 * cheap precisely because it *waits* in between. Two spinning threads cost
 * eight times what rendering Doom costs.
 *
 * No permission check. A thread choosing not to run is the one request
 * that cannot be used against anybody: it gives the machine back.
 *
 * Ticks, not the physical counter - the trap `SYS_WAIT_INPUT` documents at
 * length, and the reason this takes the same units as that one rather than
 * milliseconds, which would read as an invitation to sleep for less than
 * a tick and get a tick anyway.
 */
#define SYS_SLEEP      38   /* (ticks)                -> 0                  */

#define SYS_MAX         39

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
 * The right to make a noise.
 *
 * Its own flag rather than riding on the screen's, because they are
 * different powers: a program that draws is not thereby allowed to play
 * sound over whatever else is playing, and a program that plays a sound
 * has no business drawing. One device, one owner, one grant - the same
 * shape the screen already has, which is what makes this a line of code
 * rather than a design.
 */
#define SPAWN_AUDIO    16u

/*
 * The disk, handed on the same way and for the same reason.
 *
 * A process that can read raw sectors can read every file on the machine
 * whatever any namespace says, so this is the most powerful grant there is -
 * more than the screen, which can only draw. Exactly one process gets it:
 * the filesystem server. Everything else reaches the disk by asking that
 * server, which is what makes a namespace mean anything.
 */
#define SPAWN_DISK      4u

/*
 * Authority over every process, not only your own children.
 *
 * `SYS_KILL` is otherwise parent-only, and that rule is right: holding a
 * capability to somebody is not permission to end them. But a task manager
 * is exactly the program that needs to end things it did not start, and the
 * answer to "this program needs a power it should not have by default" in
 * this system is a grant rather than a relaxed rule.
 *
 * So: init holds it, init hands it to one process, and that process can end
 * anything. Every other process keeps the parent-only rule. What makes this
 * safe is not that killing is hard - it is that being *able* to is visible,
 * granted once, and listed by `ps` next to the console and the screen.
 */
#define SPAWN_PROCCTL   8u



/*
 * What SYS_SCHED_INFO reports: which policy is running, how long a turn is,
 * and what else this machine could be using instead.
 *
 * `tick_hz` is here so a caller can turn a quantum in ticks into
 * milliseconds without knowing what the timer was configured to. A settings
 * app that hardcoded 100 would be lying the day the tick rate changes, and
 * changing the tick rate is the only way to get a quantum below 10 ms.
 */
#define SCHED_NAME_MAX   16
#define SCHED_POLICY_MAX  4

/* What SYS_SCHED_SET changes. */
#define SCHED_SET_QUANTUM  0
#define SCHED_SET_POLICY   1

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

/*
 * Inside the guard, with every other struct in this header.
 *
 * This file is included from assembly - `user/hello.S` does, for the
 * syscall numbers - and a struct definition there is a syntax error per
 * line. The constants above may live outside because a `#define` is
 * meaningful to both; a type is not.
 */
struct schedinfo {
    uint32_t policy;        /* index of the one installed */
    uint32_t policies;      /* how many there are */
    uint32_t quantum;       /* a turn, in ticks */
    uint32_t tick_hz;       /* ticks per second */
    uint32_t priorities;    /* how many bands */
    char     name[SCHED_POLICY_MAX][SCHED_NAME_MAX];
};

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
/*
 * One process, as SYS_PROCTABLE reports it.
 *
 * `design.md` §9.5 wants this reached through `/proc` in the namespace
 * eventually, like everything else. It is a syscall today for the same
 * reason SYS_SYSINFO is: something has to be the door a server goes
 * through, and there is not yet a server to be the one.
 *
 * The kernel names nothing here and interprets nothing. A process says what
 * it is with SYS_SETNAME; what a name *means* - which layer it belongs to,
 * whether it is a server or an app - is decided in Lua, where the tables
 * that decide such things belong.
 */
struct proc_info {
    uint32_t id;
    uint32_t state;             /* the thread's: ready, running, blocked */
    uint32_t exited;
    int32_t  exit_code;
    uint64_t ticks;             /* timer ticks charged to it, only rising */
    uint32_t pages;             /* pages it holds through SYS_MAP */
    uint32_t held;              /* and everything else: image, heap, stacks */
    uint32_t caps;              /* capabilities in its table */
    uint32_t owns;              /* bit 0 the console, bit 1 the screen */

    /*
     * The band it is scheduled in - the *effective* one, so a server
     * carrying a caller's priority reports what it is actually running at
     * rather than what it was given.
     *
     * Reported because a scheduler with bands nobody can see is a scheduler
     * nobody can reason about. `scheduler` lets you change the policy and
     * the quantum while the machine runs, and until now there was no way to
     * look at what that did to any particular process.
     */
    uint32_t priority;

    char     name[16];
};

/*
 * Where the pointer is, as the device reports it.
 *
 * Undecoded, like `sysinfo`: the range travels with the position so that
 * whoever knows the size of the screen does the scaling. A kernel that
 * scaled would have to know which screen, and it does not.
 */
struct pointer_info {
    uint32_t x, y;
    uint32_t min_x, max_x;
    uint32_t min_y, max_y;
    uint32_t buttons;
    uint32_t moved;
};

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
    uint32_t processes_used;    /* running */
    uint32_t processes_held;    /* slots occupied, including unreaped exits */
    uint32_t processes_total;
    uint32_t endpoints_used;
    uint32_t endpoints_total;
    uint32_t spaces_used;
    uint32_t spaces_total;

    /*
     * Shared regions. `memobj_in_use` and `memobj_total` have existed since
     * regions did and nothing ever called them, so the pool's depth was
     * invisible - and "a region could not be allocated" is the same message
     * whether the machine is out of memory or out of *descriptors*, which
     * are very different problems. Two rounds of debugging went to the wrong
     * layer for want of this number.
     */
    uint32_t regions_used;
    uint32_t regions_total;

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

    /*
     * Seconds since 1970, from the board's clock, or 0 when it has none.
     *
     * Not a tick count, and it is the only number in here that is not.
     * Everything else says how long this machine has been doing something;
     * this says what time it is, which no counter since boot can answer -
     * a file written before the last reboot has an mtime of `sys.ticks()`
     * and that number means nothing at all across a restart.
     *
     * Read fresh on every call rather than latched at boot and added to,
     * because a load from an MMIO register is cheaper than being wrong when
     * the two drift.
     */
    uint64_t epoch;

    /*
     * The sound device's format, or zeroes when there is none.
     *
     * Reported rather than defined in a header both sides include, because
     * it is the *board's* fact: `hal.h` fixes it and a different board
     * would fix it differently. A caller sizes its buffer from what the
     * machine says it is rather than from a constant that might be stale.
     */
    uint32_t audio_rate;
    uint32_t audio_channels;
    uint32_t audio_period;      /* bytes in one period */
    uint32_t audio_periods;     /* how many the device will hold */
    uint32_t audio_dry;         /* periods that arrived at an empty device */
    uint32_t audio_floor;       /* smallest depth ever seen, in periods */
    uint32_t audio_wakes;       /* times the device raised its interrupt */

    uint32_t cpus;              /* cores the kernel is scheduling on */
    uint32_t tick_hz;
    uint32_t current_el;
    uint32_t page_size;
};

/*
 * What SYS_DISK_INFO answers.
 *
 * `sectors` is zero when there is no disk, which is a supported way to run:
 * the machine boots, and the filesystem server says it has nothing to mount
 * rather than the kernel refusing to start.
 */
struct diskinfo {
    uint64_t sectors;
    uint32_t sector_size;
    uint32_t reserved;
};

#endif

/* Errors are negative so `if (result < 0)` reads correctly on both sides. */
#define SYS_ERR_BADCALL   (-100)    /* no such syscall number */
#define SYS_ERR_FAULT     (-101)    /* a pointer the process may not touch */
#define SYS_ERR_DENIED    (-102)    /* this process does not hold the device */
#define SYS_NO_INPUT      (-103)    /* nothing waiting; not an error */
#define SYS_ERR_NO_CHILD  (-104)    /* nothing to wait for */
#define SYS_NO_CHILD_READY (-106)   /* children, but none has exited yet */
#define SYS_NO_MESSAGE    (-107)    /* nothing to receive, and not blocking */
#define SYS_ERR_NO_ROOM   (-105)    /* out of processes, or out of memory */
#define SYS_ERR_NO_CAPS   (-106)    /* this thread's capability table is full */

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
