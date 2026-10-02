/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A QEMU plugin: instructions executed, by the address of the block they
 * were in, between a start address and a stop address being reached
 * (`testing.md` 18.343, `make bench-profile`).
 *
 *   -plugin build/host/qemu_pcprof.dylib,start=0x...,stop=0x...,out=path
 *
 * **Exact, not sampled.** Under `-icount` an instruction is the unit the
 * benchmarks count in, so this says where every one of a benchmark's went:
 * every translated block gets a counter, the counter runs only inside the
 * window, and the file lists each block's address, its length and how many
 * times it ran - which `tools/prof_bench.py` adds up by function. A block
 * never spans a call, so it lies inside one function.
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

struct block { uint64_t pc; unsigned n; uint64_t runs; };

#define SLOTS (1u << 20)
static struct block *table;
static uint64_t start_pc, stop_pc;
static bool on;
static char out_path[512] = "pcprof.txt";

static struct block *find(uint64_t pc, unsigned n)
{
    uint64_t h = (pc * 0x9E3779B97F4A7C15ull ^ n) & (SLOTS - 1);

    for (;;) {
        struct block *b = &table[h];
        if (b->pc == 0) { b->pc = pc; b->n = n; return b; }
        if (b->pc == pc && b->n == n) return b;
        h = (h + 1) & (SLOTS - 1);
    }
}

static void exec(unsigned int vcpu, void *udata)
{
    struct block *b = udata;
    (void)vcpu;
    if (b->pc == start_pc) on = true;
    if (b->pc == stop_pc) on = false;
    if (on) b->runs++;
}

static void trans(struct qemu_plugin_tb *tb, void *userdata)
{
    (void)userdata;
    struct block *b = find(qemu_plugin_tb_vaddr(tb), (unsigned)qemu_plugin_tb_n_insns(tb));
    qemu_plugin_register_vcpu_tb_exec_cb(tb, exec, QEMU_PLUGIN_CB_NO_REGS, b);
}

static void done(void *p)
{
    (void)p;
    FILE *f = fopen(out_path, "w");
    if (!f) return;
    for (unsigned i = 0; i < SLOTS; i++) {
        if (table[i].runs) fprintf(f, "%" PRIx64 " %u %" PRIu64 "\n", table[i].pc, table[i].n, table[i].runs);
    }
    fclose(f);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info,
                                           int argc, char **argv)
{
    (void)info;
    table = calloc(SLOTS, sizeof(*table));
    for (int i = 0; i < argc; i++) {
        if (!strncmp(argv[i], "start=", 6)) start_pc = strtoull(argv[i] + 6, NULL, 0);
        else if (!strncmp(argv[i], "stop=", 5)) stop_pc = strtoull(argv[i] + 5, NULL, 0);
        else if (!strncmp(argv[i], "out=", 4)) snprintf(out_path, sizeof out_path, "%s", argv[i] + 4);
    }
    qemu_plugin_register_vcpu_tb_trans_cb(id, trans, NULL);
    qemu_plugin_register_atexit_cb(id, done, NULL);
    return 0;
}
