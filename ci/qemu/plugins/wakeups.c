/*
 * wakeups: QEMU TCG plugin (QEMU 4.2 plugin API, qemu-eos v4.2.1) that measures how often an
 * ARMv5 guest leaves idle. It needs no ROM-specific address, so it works for any DryOS camera
 * and for the bare-metal self-test.
 *
 * Counted:
 *   wfi    executions of a wait-for-interrupt: `mcr p15,0,Rd,c7,c0,4` (ARM926/946, the DryOS idle loop),
 *          the other pre-v7 encodings QEMU/qemu-eos treat as WFI (c7,c8,2 and c15,c8,2) and the v6K hint.
 *          Every idle entry ends with one wakeup, so wfi/s = wakeups/s.
 *   irq    executions of the IRQ vector (0x18 or high vector 0xFFFF0018) = IRQ exception entries.
 *   fiq    same for the FIQ vector (0x1C / 0xFFFF001C).
 *   insns  guest instructions (counted per executed translation block, so exact unless a block is
 *          left early by a synchronous exception).
 *   halts  times the vCPU really slept (a WFI with an interrupt already pending returns at once),
 *          and the host time spent halted.
 *
 * Derived: insns between consecutive WFIs (the work done per wakeup: mean and max) and a busy ratio.
 * With real-time QEMU (no -icount) busy_ratio = 1 - halted/elapsed host time; TCG is slower than the
 * real CPU, so this over-estimates busy time. With icount_shift=N (same N as `-icount shift=N`)
 * busy time is insns * 2^N ns over the elapsed time instead, independent of the host speed.
 * Note: -icount forces single-threaded TCG, where QEMU 4.2 calls no idle/resume plugin callbacks,
 * so halts/halted_ms stay 0 in that mode.
 *
 * Options (-plugin libwakeups.so,arg=KEY=VALUE,...):
 *   out=PATH          JSON lines output (default: stderr)
 *   start_ms=N        measurement window starts N ms (host time) after QEMU start; earlier events
 *                     (boot) are not in the summary (default 0)
 *   interval_ms=N     also emit one "interval" record every N ms (default 0: summary only)
 *   icount_shift=N    see busy ratio above (default: unset)
 *
 * Records are written at the first plugin event after each deadline (WFI, IRQ, idle/resume), so a
 * guest that never idles and never takes interrupts only gets the final "summary" record at exit.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

struct counters {
    uint64_t wfi, irq, fiq, insns, halts, halted_ns;
    uint64_t max_insns_between_wfi;
};

/* One vCPU on every machine we use (EOS cameras emulate the main ARM core only), and plugin
 * callbacks run on that vCPU thread, so plain globals need no locking. */
static struct counters now;     /* cumulative since start */
static struct counters win;     /* snapshot at the summary window start */
static struct counters ivl;     /* snapshot at the current interval start */
static uint64_t insns_at_last_wfi;
static bool have_wfi;
static uint64_t halt_start_ns;   /* 0 while running */

static uint64_t t0_ns, start_ns, interval_ns, next_interval_ns, win_t_ns, ivl_t_ns;
static bool win_started;
static int icount_shift = -1;
static FILE *out;

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec - t0_ns;
}

static void emit(const char *type, const struct counters *from, uint64_t t_from, uint64_t t_to)
{
    struct counters d = {
        .wfi = now.wfi - from->wfi, .irq = now.irq - from->irq, .fiq = now.fiq - from->fiq,
        .insns = now.insns - from->insns, .halts = now.halts - from->halts,
        .halted_ns = now.halted_ns - from->halted_ns,
    };
    double s = (t_to - t_from) / 1e9;
    double busy;

    if (icount_shift >= 0) {
        /* -icount runs single-threaded TCG, which never reports idle/resume to plugins, so the
         * halted time is unknown here; with sleep=on the virtual clock tracks host time. */
        double busy_ns = (double)d.insns * (double)(1ull << icount_shift);
        busy = t_to > t_from ? busy_ns / (t_to - t_from) : 0;
        if (busy > 1) {
            busy = 1;
        }
    } else {
        busy = t_to > t_from ? 1.0 - (double)d.halted_ns / (t_to - t_from) : 0;
    }
    fprintf(out,
            "{\"type\":\"%s\",\"t_start_ms\":%.1f,\"t_end_ms\":%.1f,\"seconds\":%.3f,"
            "\"wfi\":%" PRIu64 ",\"irq\":%" PRIu64 ",\"fiq\":%" PRIu64 ",\"insns\":%" PRIu64 ","
            "\"halts\":%" PRIu64 ",\"halted_ms\":%.1f,"
            "\"wakeups_per_s\":%.2f,\"irq_per_s\":%.2f,\"insns_per_s\":%.0f,"
            "\"insns_per_wakeup\":%.1f,\"max_insns_between_wfi\":%" PRIu64 ","
            "\"busy_ratio\":%.4f,\"busy_model\":\"%s\"}\n",
            type, t_from / 1e6, t_to / 1e6, s,
            d.wfi, d.irq, d.fiq, d.insns, d.halts, d.halted_ns / 1e6,
            s > 0 ? d.wfi / s : 0, s > 0 ? d.irq / s : 0, s > 0 ? d.insns / s : 0,
            d.wfi ? (double)d.insns / d.wfi : 0, now.max_insns_between_wfi,
            busy < 0 ? 0 : busy, icount_shift >= 0 ? "icount" : "host_time");
    fflush(out);
}

/* Window bookkeeping, called at the top of every event callback (before it counts its event). */
static void tick(void)
{
    uint64_t t = mono_ns();

    if (!win_started) {
        if (t < start_ns) {
            return;
        }
        win_started = true;
        win = now;
        win.max_insns_between_wfi = 0;
        ivl = now;
        win_t_ns = ivl_t_ns = t;
        next_interval_ns = t + interval_ns;
        now.max_insns_between_wfi = 0;      /* max is per window, boot bursts excluded */
        return;
    }
    if (interval_ns && t >= next_interval_ns) {
        emit("interval", &ivl, ivl_t_ns, t);
        ivl = now;
        ivl_t_ns = t;
        next_interval_ns = t + interval_ns;
    }
}

static void on_wfi(unsigned int vcpu, void *udata)
{
    uint64_t burst;

    tick();
    burst = now.insns - insns_at_last_wfi;
    now.wfi++;
    if (have_wfi && burst > now.max_insns_between_wfi) {
        now.max_insns_between_wfi = burst;
    }
    have_wfi = true;
    insns_at_last_wfi = now.insns;
}

static void on_irq(unsigned int vcpu, void *udata)
{
    tick();
    now.irq++;
}

static void on_fiq(unsigned int vcpu, void *udata)
{
    now.fiq++;
}

static void on_idle(qemu_plugin_id_t id, unsigned int vcpu)
{
    tick();
    now.halts++;
    halt_start_ns = mono_ns();
}

static void on_resume(qemu_plugin_id_t id, unsigned int vcpu)
{
    if (halt_start_ns) {
        now.halted_ns += mono_ns() - halt_start_ns;
        halt_start_ns = 0;
    }
    tick();
}

static bool is_wfi(uint32_t w)
{
    if ((w >> 28) == 0xF) {
        return false;                       /* MCR2 / unconditional space */
    }
    switch (w & 0x0FFF0FFF) {               /* MCR p15, opc1=0, CRn, Rd=any, opc2, CRm */
    case 0x0E070F90:                        /* c7, c0, 4: ARM926/946 WFI, DryOS idle */
    case 0x0E070F58:                        /* c7, c8, 2: older v5 cores */
    case 0x0E0F0F58:                        /* c15, c8, 2: qemu-eos variant */
        return true;
    }
    return (w & 0x0FFFFFFF) == 0x0320F003;  /* ARMv6K+ WFI hint */
}

static void on_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    size_t n = qemu_plugin_tb_n_insns(tb);

    qemu_plugin_register_vcpu_tb_exec_inline(tb, QEMU_PLUGIN_INLINE_ADD_U64, &now.insns, n);

    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint64_t va = qemu_plugin_insn_vaddr(insn);
        uint32_t w;

        if (va == 0x18 || va == 0xFFFF0018) {
            qemu_plugin_register_vcpu_insn_exec_cb(insn, on_irq, QEMU_PLUGIN_CB_NO_REGS, NULL);
        } else if (va == 0x1C || va == 0xFFFF001C) {
            qemu_plugin_register_vcpu_insn_exec_cb(insn, on_fiq, QEMU_PLUGIN_CB_NO_REGS, NULL);
        }
        /* Only 4-byte ARM instructions can be WFI on ARMv5 (Thumb-1 has no MCR). */
        if (qemu_plugin_insn_size(insn) == 4) {
            memcpy(&w, qemu_plugin_insn_data(insn), 4);    /* guest and host are little-endian */
            if (is_wfi(w)) {
                qemu_plugin_register_vcpu_insn_exec_cb(insn, on_wfi, QEMU_PLUGIN_CB_NO_REGS, NULL);
            }
        }
    }
}

static void on_atexit(qemu_plugin_id_t id, void *udata)
{
    uint64_t t = mono_ns();

    if (halt_start_ns) {                    /* exiting while halted */
        now.halted_ns += t - halt_start_ns;
        halt_start_ns = 0;
    }
    if (win_started) {
        emit("summary", &win, win_t_ns, t);
    } else {
        fprintf(out, "{\"type\":\"summary\",\"started\":false,\"t_end_ms\":%.1f,\"start_ms\":%.1f}\n",
                t / 1e6, start_ns / 1e6);
    }
    fflush(out);
    if (out != stderr) {
        fclose(out);
    }
}

static bool parse_u64(const char *s, uint64_t *v)
{
    char *end;
    *v = strtoull(s, &end, 0);
    return *s && !*end;
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info,
                                           int argc, char **argv)
{
    const char *path = NULL;
    uint64_t v;

    t0_ns = 0;
    t0_ns = mono_ns();
    out = stderr;

    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strncmp(a, "out=", 4)) {
            path = a + 4;
        } else if (!strncmp(a, "start_ms=", 9) && parse_u64(a + 9, &v)) {
            start_ns = v * 1000000u;
        } else if (!strncmp(a, "interval_ms=", 12) && parse_u64(a + 12, &v)) {
            interval_ns = v * 1000000u;
        } else if (!strncmp(a, "icount_shift=", 13) && parse_u64(a + 13, &v) && v < 32) {
            icount_shift = (int)v;
        } else {
            fprintf(stderr, "wakeups: bad option '%s'\n", a);
            return -1;
        }
    }
    if (path && !(out = fopen(path, "w"))) {
        perror(path);
        return -1;
    }

    qemu_plugin_register_vcpu_tb_trans_cb(id, on_tb_trans);
    qemu_plugin_register_vcpu_idle_cb(id, on_idle);
    qemu_plugin_register_vcpu_resume_cb(id, on_resume);
    qemu_plugin_register_atexit_cb(id, on_atexit, NULL);
    return 0;
}
