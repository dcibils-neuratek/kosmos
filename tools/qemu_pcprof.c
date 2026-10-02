/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A QEMU plugin: instructions executed, by the address of the block they
 * were in, between a start address and a stop address being reached
 * (`testing.md` 18.343, `make bench-profile`) - and, given the kernel's
 * context switch, by the thread that was running.
 *
 *   -plugin build/host/qemu_pcprof.dylib,start=0x...,stop=0x...,out=path
 *   -plugin build/host/qemu_pcprof.dylib,out=path      (all of it)
 *   -plugin build/host/qemu_pcprof.dylib,switch=0x...,names=664,out=path
 *
 * **Exact, not sampled.** Under `-icount` an instruction is the unit the
 * benchmarks count in, so this says where every one of a benchmark's went:
 * every translated block gets a counter, the counter runs only inside the
 * window, and the file lists each block's address, its length and how many
 * times it ran - which `tools/prof_bench.py` adds up by function. A block
 * never spans a call, so it lies inside one function.
 *
 * **By thread** (`testing.md` 18.344): with `switch=` the address of
 * `context_switch`, whose second argument is the context of the thread it
 * switches to, the plugin reads that one register as each switch begins and
 * charges every block after it to that thread - a register read a switch
 * rather than one a block, which is what keeps the machine usable while it
 * is measured. Each line then begins with the thread's context address. With
 * `names=` the distance from a thread's context to its name in `struct
 * thread`, each thread's name is read once, the first time it runs, and
 * listed as `# address name` lines.
 *
 * Built against the header the installed QEMU ships, so the plugin API is
 * the one the binary speaks.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

struct block { uint64_t who; uint64_t pc; unsigned n; uint64_t runs; };
struct tb { uint64_t pc; unsigned n; bool is_switch; };

#define SLOTS (1u << 22)
#define VCPUS 64

static struct block *table;
static uint64_t start_pc, stop_pc, switch_pc;
static bool on;
static uint64_t current[VCPUS];
static struct qemu_plugin_register *x1;
static GByteArray *regbuf;
static char out_path[512] = "pcprof.txt";
static uint64_t names_offset;

#define NAMED 4096
static struct { uint64_t who; char name[17]; } named[NAMED];
static unsigned named_count;

static void name_of(uint64_t who)
{
    GByteArray *buf;

    unsigned i;

    if (names_offset == 0 || who == 0) return;

    for (i = 0; i < named_count && named[i].who != who; i++) {
    }

    if (i == NAMED) return;

    buf = g_byte_array_new();

    /* Read again at every switch: a process takes its program's name after
     * it starts, so the first name a thread had is seldom its last. */
    if (qemu_plugin_read_memory_vaddr(who + names_offset, buf, 16) && buf->len >= 16) {
        named[i].who = who;
        memcpy(named[i].name, buf->data, 16);
        named[i].name[16] = '\0';

        if (i == named_count) named_count++;
    }

    g_byte_array_free(buf, TRUE);
}

static struct block *find(uint64_t who, uint64_t pc, unsigned n)
{
    uint64_t h = ((pc ^ (who * 0x9E3779B97F4A7C15ull)) * 0x9E3779B97F4A7C15ull ^ n)
                 & (SLOTS - 1);

    for (;;) {
        struct block *b = &table[h];
        if (b->pc == 0) { b->who = who; b->pc = pc; b->n = n; return b; }
        if (b->pc == pc && b->n == n && b->who == who) return b;
        h = (h + 1) & (SLOTS - 1);
    }
}

static void exec(unsigned int vcpu, void *udata)
{
    struct tb *t = udata;
    unsigned v = vcpu < VCPUS ? vcpu : VCPUS - 1;

    if (t->is_switch && x1 != NULL) {
        g_byte_array_set_size(regbuf, 0);

        if (qemu_plugin_read_register(x1, regbuf) && regbuf->len >= 8) {
            uint64_t ctx;
            memcpy(&ctx, regbuf->data, 8);
            current[v] = ctx;
            name_of(ctx);
        }
    }

    if (t->pc == start_pc) on = true;
    if (t->pc == stop_pc) on = false;
    if (on) find(current[v], t->pc, t->n)->runs++;
}

static void trans(struct qemu_plugin_tb *tb, void *userdata)
{
    struct tb *t = calloc(1, sizeof(*t));

    (void)userdata;
    t->pc = qemu_plugin_tb_vaddr(tb);
    t->n = (unsigned)qemu_plugin_tb_n_insns(tb);
    t->is_switch = switch_pc != 0 && t->pc == switch_pc;
    qemu_plugin_register_vcpu_tb_exec_cb(tb, exec, t->is_switch ? QEMU_PLUGIN_CB_R_REGS
                                                                : QEMU_PLUGIN_CB_NO_REGS, t);
}

/* The handle for x1, the second argument, found by name once. */
static void vcpu_init(unsigned int vcpu, void *userdata)
{
    GArray *regs;

    (void)vcpu; (void)userdata;

    if (switch_pc == 0 || x1 != NULL) return;

    regs = qemu_plugin_get_registers();

    for (unsigned i = 0; regs != NULL && i < regs->len; i++) {
        qemu_plugin_reg_descriptor *d = &g_array_index(regs, qemu_plugin_reg_descriptor, i);
        if (strcmp(d->name, "x1") == 0) {
            x1 = d->handle;
        }
    }

    if (regs != NULL) g_array_free(regs, TRUE);
}

static void done(void *p)
{
    (void)p;
    FILE *f = fopen(out_path, "w");
    if (!f) return;
    for (unsigned i = 0; i < named_count; i++) {
        fprintf(f, "# %" PRIx64 " %s\n", named[i].who, named[i].name);
    }
    for (unsigned i = 0; i < SLOTS; i++) {
        if (!table[i].runs) continue;
        if (switch_pc != 0) {
            fprintf(f, "%" PRIx64 " ", table[i].who);
        }
        fprintf(f, "%" PRIx64 " %u %" PRIu64 "\n", table[i].pc, table[i].n, table[i].runs);
    }
    fclose(f);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info,
                                           int argc, char **argv)
{
    (void)info;
    table = calloc(SLOTS, sizeof(*table));
    regbuf = g_byte_array_new();
    for (int i = 0; i < argc; i++) {
        if (!strncmp(argv[i], "start=", 6)) start_pc = strtoull(argv[i] + 6, NULL, 0);
        else if (!strncmp(argv[i], "stop=", 5)) stop_pc = strtoull(argv[i] + 5, NULL, 0);
        else if (!strncmp(argv[i], "switch=", 7)) switch_pc = strtoull(argv[i] + 7, NULL, 0);
        else if (!strncmp(argv[i], "names=", 6)) names_offset = strtoull(argv[i] + 6, NULL, 0);
        else if (!strncmp(argv[i], "out=", 4)) snprintf(out_path, sizeof out_path, "%s", argv[i] + 4);
    }
    /* No window given: everything, from the first instruction. */
    on = (start_pc == 0);
    qemu_plugin_register_vcpu_init_cb(id, vcpu_init, NULL);
    qemu_plugin_register_vcpu_tb_trans_cb(id, trans, NULL);
    qemu_plugin_register_atexit_cb(id, done, NULL);
    return 0;
}
