/** \file
 * Read-only capture kit for the EOS M 2.0.2 (ML_PROFILE=capture, docs/CAPTURE-KIT.md).
 *
 * Collects data from the real camera for the emulator, and nothing else:
 *   - boot log: Canon's DryosDebugMsg and the MPU messages from power-on (RAM only, saved from the menu)
 *   - serial flash dump (Canon's own READ routine, opcode 0x03 only)
 *   - logging session: DryosDebugMsg + MPU messages (both directions) + power-management samples
 *   - LiveView buffer dumps, RAM dumps after a countdown (so the camera can be put in a given state)
 * Everything goes to ML/LOGS/ on the card.
 *
 * What it never does (enforced here, in internals-capture.h / stubs.S and by tools/check_capture_kit.py):
 * property writes (prop_request_change is a logging no-op), Canon settings or custom functions,
 * image-engine / audio register writes, serial flash or ROM program/erase, boot flag changes,
 * firmware update, eventprocs other than Disable/EnablePowerSave, module loading.
 *
 * The only Canon state it changes: one instruction of DryosDebugMsg in RAM (a branch to our logger,
 * verified against the expected original and undone when logging stops), and the CPU power-save
 * counter while a dump is written to the card (a counted hold, released when the dump ends).
 */

#include "dryos.h"
#include "menu.h"
#include "bmp.h"
#include "fio-ml.h"
#include "module.h"
#include "patch.h"
#include "powersave.h"
#include "propvalues.h"
#include "timer.h"
#include "vram.h"
#include "arm-mcr.h"

extern int ml_started;

/* ---- build-time guarantees ------------------------------------------------------------------------- */
#ifndef CONFIG_PROFILE_CAPTURE
#error "capture-kit.c is only built with ML_PROFILE=capture"
#endif
#if !defined(CONFIG_EOSM) || CONFIG_FW_VERSION != 202
#error "the capture kit addresses are for the EOS M firmware 2.0.2 only"
#endif
#if defined(CONFIG_PROP_REQUEST_CHANGE) || defined(CONFIG_PROP_WRITE_WHITELIST) || defined(CONFIG_MODULES) \
 || defined(CONFIG_RAW_LIVEVIEW) || defined(CONFIG_RAW_PHOTO) || defined(CONFIG_FRAME_ISO_OVERRIDE) \
 || defined(CONFIG_FRAME_SHUTTER_OVERRIDE) || defined(CONFIG_RESTORE_AFTER_FORMAT) || defined(CONFIG_AUTOBACKUP_ROM) \
 || defined(CONFIG_DIGIC_POKE) || defined(CONFIG_DISPLAY_FILTERS) || defined(CONFIG_EXPSIM) \
 || defined(CONFIG_CAN_REDIRECT_DISPLAY_BUFFER) || defined(CONFIG_LVAPP_HACK_DEBUGMSG)
#error "a write-capable internal is enabled in the capture profile (see platform/EOSM.202/internals-capture.h)"
#endif
#if defined(FEATURE_BOOTFLAG_MENU) || defined(FEATURE_DONT_CLICK_ME) || defined(FEATURE_FPS_OVERRIDE) \
 || defined(FEATURE_CROP_MODE_HACK) || defined(FEATURE_EXPO_OVERRIDE) || defined(FEATURE_PICSTYLE) \
 || defined(FEATURE_WHITE_BALANCE) || defined(FEATURE_INTERVALOMETER) || defined(FEATURE_HDR_BRACKETING) \
 || defined(FEATURE_SHOW_EDMAC_INFO) || defined(FEATURE_UNMOUNT_SD_CARD) || defined(FEATURE_VIDEO_HACKS)
#error "a normal-build feature leaked into the capture profile (see platform/EOSM.202/features-capture.h)"
#endif

/* ---- firmware 2.0.2 addresses (evidence: docs/CAPTURE-KIT.md; each one is verified at run time) --------- */

/* Serial flash, Canon's own read path (the one its sf_gang eventproc uses for an 8 MiB checksum):
 * SF_CreateSerial opens the SIO channel (takes Canon's SIO semaphore), readSerialFlash sends
 * READ (0x03) + 24-bit address and clocks in `size` bytes, SF_Destroy closes the channel. */
#define SF_CREATE_SERIAL        0xFF1385AC
#define SF_READ_SERIAL_FLASH    0xFF138554
#define SF_DESTROY              0xFF13AF5C
#define SF_SIZE                 0x800000        /* sf_gang reads 0x800000 bytes */
#define SF_CHUNK                0x10000

/* MPU communication (SIO3), RAM copy of the kernel: state struct, send ring (20 x 0x80) and receive ring
 * (60 x 0x80) allocated by the Intercom init (0x1DC54); mpu_send = 0x1DD2C ("dwSize < TXBD_DATA_SIZE"). */
#define MPU_STATE               0x4C5BC         /* +0 recv index, +4 recv byte offset, +8 send index */
#define MPU_SEND_RING           0x7E4F0
#define MPU_SEND_N              20
#define MPU_RECV_RING           0x7E400
#define MPU_RECV_N              60

/* Power management (docs: QEMU-EOS-Optimized docs/models/EOSM.md, canon-firmware-re notes) */
#define PM_COUNTER              0x3EE5C         /* Disable/EnablePowerSave counter */
#define PM_STATE                0x4D750         /* PowerMgr state: +0x0C mode, +0x10 self-refresh flag, +0x20 stamp */
#define CLOCK_ENABLE            0xC0400008      /* read by Canon's PowerMgr loop on every idle pass */
#define LCLK_MASK               0xC0F01004      /* read by Canon's PowerMgr loop on every idle pass */
/* Eeko (second core) clock/reset control, written by Canon when EekoNavigator starts/stops it (7 = run, 6 = clock
 * on in reset, 0 = off; C0223210 = 0x7E before a start). Plain control registers, sampled with 32-bit reads only;
 * the mailbox FIFOs of the same block (read = pop) are never touched. */
#define EEKO_CLK_RESET          0xC022320C
#define EEKO_CLK_DIV            0xC0223210

#define DM_ADDR                 0x40D4          /* DryosDebugMsg, RAM copy (stubs.S) */

struct code_sig { uint32_t addr; uint32_t value; const char * what; };

static const struct code_sig sf_sigs[] = {
    { 0xFF1385AC, 0xE92D407C, "SF_CreateSerial: push {r2-r6,lr}" },
    { 0xFF1385B8, 0xEB000B8A, "SF_CreateSerial: bl SIO open (0xFF13B3E8, takes the SIO semaphore)" },
    { 0xFF138554, 0xE92D41FC, "readSerialFlash: push {r2-r8,lr}" },
    { 0xFF138584, 0xEB000A7B, "readSerialFlash: bl SetCS (0xFF13AF78)" },
    { 0xFF13858C, 0xE3A00003, "readSerialFlash: mov r0, #3 (READ opcode)" },
    { 0xFF138590, 0xEBFFFFDB, "readSerialFlash: bl SendCommandWithAddress (0xFF138504)" },
    { 0xFF13859C, 0xEBFFFEC7, "readSerialFlash: bl receive (0xFF1380C0)" },
    { 0xFF1385A8, 0xEA000A72, "readSerialFlash: b SetCS (0xFF13AF78)" },
    { 0xFF13AF5C, 0xE92D4010, "SF_Destroy: push {r4,lr}" },
    { 0xFF13AF68, 0xEB000139, "SF_Destroy: bl SIO close (0xFF13B454)" },
};

static const struct code_sig mpu_sigs[] = {
    { 0x1DD2C, 0xE92D41F0, "mpu_send: push {r4-r8,lr}" },
    { 0x1DEF8, MPU_STATE,     "SIO3 state pointer literal" },
    { 0x1DEFC, MPU_SEND_RING, "send ring literal" },
    { 0x1DF04, MPU_RECV_RING, "receive ring literal" },
    { 0x1DCA0, 0xEBFFFF50,    "Intercom init: bl alloc ring (send)" },
    { 0x1DC9C, 0xE3A01014,    "Intercom init: mov r1, #20 (send ring size)" },
    { 0x1DC80, 0xE3A0103C,    "Intercom init: mov r1, #60 (receive ring size)" },
};

static const struct code_sig dm_sigs[] = {
    { DM_ADDR, 0xE92D000F, "DryosDebugMsg: push {r0-r3}" },
};

static int sigs_ok(const struct code_sig * s, int n, char * why, int why_len)
{
    for (int i = 0; i < n; i++)
    {
        uint32_t v = *(volatile uint32_t *) s[i].addr;
        if (v != s[i].value)
        {
            if (why) snprintf(why, why_len, "%08x = %08x, expected %08x (%s)", s[i].addr, v, s[i].value, s[i].what);
            return 0;
        }
    }
    return 1;
}

/* ---- blocked write paths ---------------------------------------------------------------------------- */

static volatile int blocked_count = 0;
static char blocked_last[48] = "";

void capture_blocked(const char * what, uint32_t a, uint32_t b)
{
    blocked_count++;
    snprintf(blocked_last, sizeof(blocked_last), "%s(%x,%x)", what, a, b);
    DryosDebugMsg(0, 15, "[CAPTURE] blocked %s(%x, %x)", what, a, b);
}

/* The Canon stubs below are not assembled in this profile (stubs.S, WSTUB); the ML wrappers that would
 * call them (EngDrvOut, engio_write, set_htp...) land here instead. */
void _EngDrvOut(uint32_t reg, uint32_t value) { capture_blocked("EngDrvOut", reg, value); }
void _engio_write(uint32_t * reg_list) { capture_blocked("engio_write", (uint32_t) reg_list, 0); }
void SetCFnData(int group, int number, int value) { capture_blocked("SetCFnData", group << 16 | number, value); }

/* Eventprocs by name: only the CPU power-save counter may be touched. */
extern int _call_unfiltered(const char * name, ...);
int call(const char * name, ...)
{
    static const char * const allowed[] = { "DisablePowerSave", "EnablePowerSave" };
    for (int i = 0; i < COUNT(allowed); i++)
    {
        if (streq(name, allowed[i]))
        {
            return _call_unfiltered(name);
        }
    }
    capture_blocked(name, 0, 0);
    return -1;
}

/* no modules in this profile */
int module_exec_cbr(unsigned int type) { return CBR_RET_CONTINUE; }

/* ---- time ------------------------------------------------------------------------------------------- */

/* microseconds since the first call, from the 20-bit DIGIC timer; called at least every 10 ms while
 * anything is logging (poller task), so wraps (every 1.05 s) are never missed. Call with IRQs off. */
static uint32_t ts_hi = 0, ts_last = 0;
static uint32_t ts_now_nolock(void)
{
    uint32_t raw = GET_DIGIC_TIMER() & (DIGIC_TIMER_MAX - 1);
    if (raw < ts_last) ts_hi += DIGIC_TIMER_MAX;
    ts_last = raw;
    return ts_hi + raw;
}
static uint32_t ts_now(void)
{
    uint32_t old = cli();
    uint32_t t = ts_now_nolock();
    sei(old);
    return t;
}

/* ---- log buffers ------------------------------------------------------------------------------------ */

struct logbuf { char * buf; uint32_t size; volatile uint32_t len; volatile uint32_t lost; };

static void lb_vappend(struct logbuf * lb, uint32_t ts, const char * prefix, const char * fmt, va_list ap)
{
    uint32_t old = cli();
    if (lb->buf && lb->len + 320 < lb->size)
    {
        char * p = lb->buf + lb->len;
        uint32_t room = lb->size - lb->len - 2;
        int n = snprintf(p, room, "%u.%06u %s", ts / 1000000, ts % 1000000, prefix);
        if (n > 0 && (uint32_t) n < room)
        {
            int m = vsnprintf(p + n, room - n, fmt, ap);
            if (m > 0) n += MIN((uint32_t) m, room - n - 1);
        }
        if (n > 0)
        {
            p[n++] = '\n';
            lb->len += n;
        }
    }
    else if (lb->buf)
    {
        lb->lost++;
    }
    sei(old);
}

static void lb_append(struct logbuf * lb, const char * prefix, const char * fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    lb_vappend(lb, ts_now(), prefix, fmt, ap);
    va_end(ap);
}

static int ensure_logs_dir(void)
{
    if (!is_dir("ML/LOGS")) FIO_CreateDirectory("ML/LOGS");
    return is_dir("ML/LOGS");
}

/* free space in MiB on the ML card */
static int free_mib(void)
{
    return get_free_space_32k(get_ml_card()) / 32;
}

static int save_file(const char * name, const void * data, uint32_t len)
{
    if (!ensure_logs_dir()) return -1;
    FILE * f = FIO_CreateFile(name);
    if (!f) return -1;
    int w = len ? FIO_WriteFile(f, data, len) : 0;
    FIO_CloseFile(f);
    return (w == (int) len) ? 0 : -1;
}

static int numbered(const char * pattern, char * out, int len)
{
    return get_numbered_file_name(pattern, 99, out, len);
}

/* ---- DryosDebugMsg logger --------------------------------------------------------------------------- */

/* Until ML's memory backend is up (a few seconds after power-on) the boot capture writes into these
 * static buffers (kept small: the kit's whole footprint stays below the full ML build's); capture_init()
 * then moves it to larger heap buffers (boot_grow). */
static char boot_dm_mem[128 * 1024];
static char boot_mpu_mem[32 * 1024];
static struct logbuf boot_dm  = { boot_dm_mem,  sizeof(boot_dm_mem),  0, 0 };
static struct logbuf boot_mpu = { boot_mpu_mem, sizeof(boot_mpu_mem), 0, 0 };
static struct logbuf sess_dm, sess_mpu, sess_pm;

static struct logbuf * volatile dm_target = 0;     /* where DryosDebugMsg lines go (0 = nowhere) */
static struct logbuf * volatile mpu_target = 0;    /* where MPU lines go */
static struct logbuf * volatile pm_target = 0;     /* where power-management samples go */
static int dm_patched = 0;
static char dm_error[96] = "";

static void capture_DebugMsg(int class, int level, char * fmt, ...)
{
    struct logbuf * lb = dm_target;
    if (!lb || !fmt) return;
    if (class == 21) return;    /* engio: one line per register write, floods the buffer (as upstream dm-spy) */

    char prefix[40];
    uint32_t old = cli();
    uint32_t ts = ts_now_nolock();
    snprintf(prefix, sizeof(prefix), "%-16.16s %02x %02x ", get_current_task_name(), class & 0xFF, level & 0xFF);
    va_list ap;
    va_start(ap, fmt);
    lb_vappend(lb, ts, prefix, fmt, ap);
    va_end(ap);
    sei(old);
}

static struct patch dm_patch;

static int dm_hook_install(void)
{
    if (dm_patched) return 0;
    if (!sigs_ok(dm_sigs, COUNT(dm_sigs), dm_error, sizeof(dm_error))) return -1;
    dm_patch = (struct patch) {
        .addr = (uint8_t *) DM_ADDR,
        .old_value = 0xE92D000F,
        .new_value = B_INSTR(DM_ADDR, capture_DebugMsg),
        .size = 4,
        .description = "capture kit: DryosDebugMsg logger",
        .is_instruction = 1,
    };
    int err = apply_patches(&dm_patch, 1);
    if (err)
    {
        snprintf(dm_error, sizeof(dm_error), "patch error %x", err);
        return -1;
    }
    dm_patched = 1;
    return 0;
}

static void dm_hook_remove(void)
{
    if (!dm_patched) return;
    dm_target = 0;
    unpatch_memory(DM_ADDR);
    dm_patched = 0;
}

/* ---- MPU ring poller (pure RAM reads, no hook) ---------------------------------------------------------- */

static int mpu_ok = 0;
static char mpu_error[96] = "";
static int mpu_send_last = -1, mpu_recv_last = -1;
static uint32_t mpu_sent = 0, mpu_recvd = 0;

static void mpu_log_msg(char dir, const uint8_t * msg)
{
    /* send buffer: [2] queued flag, [3] = [4] = even wire size; recv buffer: [3] = size; wire bytes from [4] */
    int n = msg[3];
    if (n <= 0 || n > 0x7C) return;
    char hex[0x7C * 3 + 1];
    int k = 0;
    for (int i = 0; i < n; i++)
    {
        k += snprintf(hex + k, sizeof(hex) - k, "%02x ", msg[4 + i]);
    }
    if (k) hex[k - 1] = 0;
    struct logbuf * lb = mpu_target;
    if (lb) lb_append(lb, dir == 'S' ? "S " : "R ", "%s", hex);
    struct logbuf * dl = dm_target;
    if (dl) lb_append(dl, dir == 'S' ? "[MPU] mpu_send(" : "[MPU] mpu_recv(", "%s)", hex);
}

static void mpu_poll(void)
{
    if (!mpu_ok) return;
    uint32_t * send_ring = (uint32_t *) MPU_SEND_RING;
    uint32_t * recv_ring = (uint32_t *) MPU_RECV_RING;
    if (!send_ring[0] || !recv_ring[0]) return;     /* Intercom not initialised yet */

    int send_idx = MEM(MPU_STATE + 8);
    int recv_idx = MEM(MPU_STATE + 0);
    if (send_idx < 0 || send_idx >= MPU_SEND_N || recv_idx < 0 || recv_idx >= MPU_RECV_N) return;

    if (mpu_send_last < 0) mpu_send_last = 0;       /* first look: everything since the Intercom init */
    if (mpu_recv_last < 0) mpu_recv_last = 0;

    while (mpu_send_last != send_idx)
    {
        const uint8_t * m = (const uint8_t *) send_ring[mpu_send_last];
        if (m) mpu_log_msg('S', m);
        mpu_sent++;
        mpu_send_last = (mpu_send_last + 1) % MPU_SEND_N;
    }
    while (mpu_recv_last != recv_idx)
    {
        const uint8_t * m = (const uint8_t *) recv_ring[mpu_recv_last];
        if (m) mpu_log_msg('R', m);
        mpu_recvd++;
        mpu_recv_last = (mpu_recv_last + 1) % MPU_RECV_N;
    }
}

/* power management: RAM state + two registers Canon's own idle loop reads on every pass */
static uint32_t pm_prev[8] = { 0xFFFFFFFF };
static void pm_poll(void)
{
    struct logbuf * lb = pm_target;
    if (!lb) return;
    uint32_t v[8] = {
        MEM(PM_COUNTER), MEM(PM_STATE + 0x0C), MEM(PM_STATE + 0x10), MEM(PM_STATE + 0x20),
        MEM(CLOCK_ENABLE), MEM(LCLK_MASK), MEM(EEKO_CLK_RESET), MEM(EEKO_CLK_DIV),
    };
    if (memcmp(v, pm_prev, sizeof(v)) == 0) return;
    memcpy(pm_prev, v, sizeof(v));
    lb_append(lb, "PM ", "counter=%d mode=%d sr_flag=%d sr_stamp=%d clock_enable=%08x lclk_mask=%08x "
        "eeko_ctl=%08x eeko_div=%08x lv=%d disp=%d", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], lv, DISPLAY_IS_ON);
}

/* ---- poller task: boot capture, sessions ------------------------------------------------------------ */

#define BOOT_CAPTURE_MS 60000
static volatile int boot_active = 0;           /* boot capture running */
static volatile int boot_grown = 0;            /* boot buffers moved to the heap (capture_init) */
static volatile int session_active = 0;
static volatile int session_stop_req = 0;
static int session_end_ms = 0;
static int session_seconds = 0;
static char status_msg[96] = "";

static void capture_poll_task(void * unused)
{
    int start = get_ms_clock();
    while (1)
    {
        ts_now();
        mpu_poll();
        pm_poll();

        if (boot_active)
        {
            int full = boot_grown && (boot_dm.lost || boot_mpu.lost);
            if (full || get_ms_clock() - start > BOOT_CAPTURE_MS)
            {
                if (dm_target == &boot_dm) dm_target = 0;
                if (mpu_target == &boot_mpu) mpu_target = 0;
                if (!session_active) dm_hook_remove();
                boot_active = 0;
                DryosDebugMsg(0, 15, "[CAPTURE] boot capture done: dm %d bytes, mpu %d bytes%s",
                    boot_dm.len, boot_mpu.len, full ? " (buffer full)" : "");
            }
        }
        msleep(10);
    }
}

/* Called right after Canon's init_task (init.c, boot_post_init_task): capture from power-on. */
void capture_boot_start(void)
{
    mpu_ok = sigs_ok(mpu_sigs, COUNT(mpu_sigs), mpu_error, sizeof(mpu_error));
    ts_now();
    mpu_target = &boot_mpu;
    dm_target = &boot_dm;
    if (dm_hook_install() != 0) dm_target = 0;
    boot_active = 1;
    task_create("capture_poll", 0x19, 0x1000, capture_poll_task, 0);
}

/* ---- menu actions ----------------------------------------------------------------------------------- */

static void say(const char * fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status_msg, sizeof(status_msg), fmt, ap);
    va_end(ap);
    NotifyBox(4000, "%s", status_msg);
    DryosDebugMsg(0, 15, "[CAPTURE] %s", status_msg);
}

static void save_boot_logs(void)
{
    if (boot_active)
    {
        say("Boot capture still running (%ds), retry later", BOOT_CAPTURE_MS / 1000);
        return;
    }
    int a = save_file("ML/LOGS/BOOTDM.LOG", boot_dm.buf, boot_dm.len);
    int b = save_file("ML/LOGS/BOOTMPU.LOG", boot_mpu.buf, boot_mpu.len);
    char info[160];
    int n = snprintf(info, sizeof(info), "boot capture: dm %d bytes, %d lines lost; mpu %d bytes, %d lines lost; "
        "mpu sent %d recv %d; blocked writes %d\n", boot_dm.len, boot_dm.lost, boot_mpu.len, boot_mpu.lost,
        mpu_sent, mpu_recvd, blocked_count);
    save_file("ML/LOGS/BOOTINFO.TXT", info, n);
    say("Boot logs %s: DM %d KB (%d lost), MPU %d KB", (a || b) ? "NOT saved" : "saved",
        boot_dm.len / 1024, boot_dm.lost, boot_mpu.len / 1024);
}

static int sf_dump(void)
{
    char why[96];
    if (!sigs_ok(sf_sigs, COUNT(sf_sigs), why, sizeof(why)))
    {
        say("SF dump refused: %s", why);
        return -1;
    }
    if (free_mib() < SF_SIZE / 1024 / 1024 + 4)
    {
        say("SF dump: not enough space on the card");
        return -1;
    }
    if (!ensure_logs_dir()) return -1;

    uint8_t * buf = fio_malloc(SF_CHUNK);
    if (!buf) { say("SF dump: out of memory"); return -1; }
    FILE * f = FIO_CreateFile("ML/LOGS/SFDATA.BIN");
    if (!f) { fio_free(buf); say("SF dump: cannot create SFDATA.BIN"); return -1; }

    int (*SF_CreateSerial)(void) = (void *) SF_CREATE_SERIAL;
    int (*SF_readSerialFlash)(int src, void * dest, int size) = (void *) SF_READ_SERIAL_FLASH;
    int (*SF_Destroy)(void) = (void *) SF_DESTROY;

    powersave_hold("capture_sf");
    int err = 0;
    for (int addr = 0; addr < SF_SIZE; addr += SF_CHUNK)
    {
        /* one open/read/close per chunk: Canon's own users of the SIO channel get it between chunks */
        SF_CreateSerial();
        SF_readSerialFlash(addr, buf, SF_CHUNK);
        SF_Destroy();
        if (FIO_WriteFile(f, buf, SF_CHUNK) != SF_CHUNK) { err = 1; break; }
        bmp_printf(FONT_MED, 20, 60, "Serial flash: %3d%%", (addr + SF_CHUNK) * 100 / SF_SIZE);
    }
    powersave_release("capture_sf");
    FIO_CloseFile(f);
    fio_free(buf);
    say(err ? "SF dump FAILED (card write)" : "SFDATA.BIN saved (8 MiB, READ 0x03 only)");
    return err ? -1 : 0;
}

static int alloc_log(struct logbuf * lb, uint32_t size)
{
    lb->len = lb->lost = 0;
    lb->size = 0;
    for (uint32_t s = size; s >= 64 * 1024; s /= 2)
    {
        lb->buf = malloc(s);
        if (lb->buf) { lb->size = s; return 0; }
    }
    return -1;
}

static void free_log(struct logbuf * lb)
{
    if (lb->buf) free(lb->buf);
    lb->buf = 0;
    lb->size = lb->len = 0;
}

static void session_task(void * unused)
{
    if (alloc_log(&sess_dm, 4 * 1024 * 1024) || alloc_log(&sess_mpu, 512 * 1024) || alloc_log(&sess_pm, 256 * 1024))
    {
        free_log(&sess_dm); free_log(&sess_mpu); free_log(&sess_pm);
        say("Session: out of memory");
        session_active = 0;
        return;
    }
    session_stop_req = 0;
    session_end_ms = get_ms_clock() + session_seconds * 1000;
    memset(pm_prev, 0xFF, sizeof(pm_prev));

    uint32_t old = cli();
    pm_target = &sess_pm;
    mpu_target = &sess_mpu;
    dm_target = &sess_dm;
    sei(old);
    if (!dm_patched && dm_hook_install() != 0)
    {
        dm_target = 0;
    }
    lb_append(&sess_dm, "[CAPTURE] ", "session start, %d s, dm hook %s", session_seconds, dm_patched ? "on" : dm_error);
    say("Logging %d s: close the menu and go (see checklist)", session_seconds);

    while (!session_stop_req && get_ms_clock() < session_end_ms
           && !sess_dm.lost && !sess_mpu.lost)
    {
        msleep(100);
    }
    const char * why = session_stop_req ? "stopped from the menu" : sess_dm.lost || sess_mpu.lost ? "buffer full" : "time over";
    lb_append(&sess_dm, "[CAPTURE] ", "session end (%s)", why);

    old = cli();
    pm_target = 0;
    mpu_target = boot_active ? &boot_mpu : 0;
    dm_target = boot_active ? &boot_dm : 0;
    sei(old);
    if (!boot_active) dm_hook_remove();

    char dmn[32], mpun[32], pmn[32];
    int num = MAX(0, numbered("ML/LOGS/DM%02d.LOG", dmn, sizeof(dmn)));
    snprintf(mpun, sizeof(mpun), "ML/LOGS/MPU%02d.LOG", num);
    snprintf(pmn, sizeof(pmn), "ML/LOGS/PM%02d.LOG", num);
    powersave_hold("capture_log");
    int e = save_file(dmn, sess_dm.buf, sess_dm.len) | save_file(mpun, sess_mpu.buf, sess_mpu.len)
          | save_file(pmn, sess_pm.buf, sess_pm.len);
    powersave_release("capture_log");
    say("Session %02d %s (%s): DM %d KB, MPU %d KB", num, e ? "NOT saved" : "saved", why,
        sess_dm.len / 1024, sess_mpu.len / 1024);
    free_log(&sess_dm); free_log(&sess_mpu); free_log(&sess_pm);
    session_active = 0;
}

static int session_choice = 2;                  /* index into session_lengths */
static const int session_lengths[] = { 30, 60, 120, 300, 600 };

static void session_toggle(void * priv, int delta)
{
    if (session_active)
    {
        session_stop_req = 1;
        return;
    }
    session_seconds = session_lengths[session_choice];
    session_active = 1;
    task_create("capture_session", 0x1c, 0x2000, session_task, 0);
}

static int lv_dump(int frames)
{
    if (!ensure_logs_dir()) return -1;
    if (free_mib() < frames * 3 + 4) { say("LV dump: not enough space"); return -1; }
    char info[1024];
    int k = snprintf(info, sizeof(info), "lv=%d disp=%d gui_menu=%d\n", lv, DISPLAY_IS_ON, gui_menu_shown());
    powersave_hold("capture_lv");
    for (int i = 0; i < frames; i++)
    {
        struct vram_info * v = get_yuv422_vram();
        struct vram_info * h = get_yuv422_hd_vram();
        char name[32];
        int e1 = -1, e2 = -1;
        if (v && v->vram)
        {
            snprintf(name, sizeof(name), "ML/LOGS/LV%d.422", i);
            e1 = save_file(name, v->vram, v->pitch * v->height);
        }
        if (h && h->vram)
        {
            snprintf(name, sizeof(name), "ML/LOGS/HD%d.422", i);
            e2 = save_file(name, h->vram, h->pitch * h->height);
        }
        k += snprintf(info + k, sizeof(info) - k,
            "frame %d t=%u: LV %p %dx%d pitch %d %s; HD %p %dx%d pitch %d %s; display buf %08x, HD DMA %08x\n", i, ts_now(),
            v ? v->vram : 0, v ? v->width : 0, v ? v->height : 0, v ? v->pitch : 0, e1 ? "-" : "saved",
            h ? h->vram : 0, h ? h->width : 0, h ? h->height : 0, h ? h->pitch : 0, e2 ? "-" : "saved",
            YUV422_LV_BUFFER_DISPLAY_ADDR, YUV422_HD_BUFFER_DMA_ADDR);
        msleep(300);
    }
    powersave_release("capture_lv");
    int e = save_file("ML/LOGS/LVINFO.TXT", info, k);
    say("LiveView buffers %s (%d frames)", e ? "NOT saved" : "saved", frames);
    return e;
}

static int ram_label = 0;
static const char * const ram_labels[] = { "LV", "MENU", "PLAY", "DOFF", "OTHR" };
static int ram_delay = 1;
static const int ram_delays[] = { 5, 15, 30, 60 };

static int ram_dump(const char * label, int delay_s, uint32_t size)
{
    if (!ensure_logs_dir()) return -1;
    if (free_mib() < (int) (size >> 20) + 8) { say("RAM dump: not enough space (%d MiB free)", free_mib()); return -1; }
    for (int i = delay_s; i > 0; i--)
    {
        NotifyBox(1100, "RAM dump (%s) in %d s", label, i);
        msleep(1000);
    }
    char name[32], txt[32], info[256];
    snprintf(name, sizeof(name), "ML/LOGS/RAM_%s.BIN", label);
    snprintf(txt, sizeof(txt), "ML/LOGS/RAM_%s.TXT", label);
    int n = snprintf(info, sizeof(info), "start 0x40000000 size 0x%x (uncached view of DRAM 0..)\n"
        "t=%u lv=%d disp=%d gui_menu=%d pm_counter=%d pm_mode=%d\n",
        size, ts_now(), lv, DISPLAY_IS_ON, gui_menu_shown(), MEM(PM_COUNTER), MEM(PM_STATE + 0x0C));
    FILE * f = FIO_CreateFile(name);
    if (!f) { say("RAM dump: cannot create %s", name); return -1; }
    powersave_hold("capture_ram");
    int err = 0;
    for (uint32_t off = 0; off < size; off += 0x1000000)
    {
        uint32_t chunk = MIN(0x1000000, size - off);
        if (FIO_WriteFile(f, (const void *) (0x40000000 + off), chunk) != (int) chunk) { err = 1; break; }
    }
    FIO_CloseFile(f);
    powersave_release("capture_ram");
    save_file(txt, info, n);
    say("%s %s (%d MiB)", name + 8, err ? "FAILED" : "saved", size >> 20);
    return err ? -1 : 0;
}

static void ram_dump_menu_task(void * unused)
{
    ram_dump(ram_labels[ram_label], ram_delays[ram_delay], 0x10000000);
}

static void lv_dump_menu_task(void * unused) { lv_dump(4); }
static void sf_dump_menu_task(void * unused) { gui_stop_menu(); msleep(500); sf_dump(); }
static void boot_logs_menu_task(void * unused) { save_boot_logs(); }

/* Emulation self-test: runs every capture once if ML/CAPTEST.TXT is on the card (CI only, see
 * ci/qemu/capture_kit.py). The same read-only actions as the menu, with a short session and a
 * small RAM dump; ends with "[CAPTURE] selftest done" in the debug log and ML/LOGS/CAPTEST.OK. */
static void capture_selftest_task(void * unused)
{
    while (!ml_started || boot_active) msleep(500);
    DryosDebugMsg(0, 15, "[CAPTURE] selftest start");
    save_boot_logs();
    int e = 0;
    e |= sf_dump() ? 1 : 0;
    session_seconds = 20;
    session_active = 1;
    session_task(0);
    e |= lv_dump(2) ? 2 : 0;
    e |= ram_dump("OTHR", 1, 16 << 20) ? 4 : 0;
    char ok[96];
    int n = snprintf(ok, sizeof(ok), "selftest errors=%d blocked=%d last=%s\n", e, blocked_count, blocked_last);
    save_file("ML/LOGS/CAPTEST.OK", ok, n);
    DryosDebugMsg(0, 15, "[CAPTURE] selftest done errors=%d blocked=%d", e, blocked_count);
}

/* ---- menu ------------------------------------------------------------------------------------------- */

static MENU_UPDATE_FUNC(status_update)
{
    MENU_SET_VALUE("%s", (mpu_ok && dm_patched) || (mpu_ok && !boot_active) ? "OK" : "check");
    MENU_SET_WARNING(MENU_WARN_INFO, "MPU ring %s%s. DM hook: %s. Blocked writes: %d. Free %d MiB.",
        mpu_ok ? "OK" : "BAD ", mpu_ok ? "" : mpu_error, dm_patched ? "on" : dm_error[0] ? dm_error : "off",
        blocked_count, free_mib());
}

static MENU_UPDATE_FUNC(boot_update)
{
    MENU_SET_VALUE("%s", boot_active ? "recording" : "ready");
    MENU_SET_RINFO("DM %dK MPU %dK", boot_dm.len / 1024, boot_mpu.len / 1024);
}

static MENU_UPDATE_FUNC(session_update)
{
    if (session_active)
    {
        MENU_SET_NAME("Stop & save session");
        MENU_SET_VALUE("%ds left", MAX(0, (session_end_ms - get_ms_clock()) / 1000));
    }
    else
    {
        MENU_SET_VALUE("%ds", session_lengths[session_choice]);
    }
}

static struct menu_entry capture_menu[] =
{
    {
        .name   = "Kit status",
        .update = status_update,
        .icon_type = IT_ALWAYS_ON,
        .help   = "Read-only capture kit. Checks of the firmware routines it uses.",
    },
    {
        .name   = "1. Save boot logs",
        .select = run_in_separate_task,
        .priv   = boot_logs_menu_task,
        .update = boot_update,
        .icon_type = IT_ACTION,
        .help   = "DryosDebugMsg + MPU messages of the first 60 s after power-on.",
        .help2  = "-> ML/LOGS/BOOTDM.LOG, BOOTMPU.LOG",
    },
    {
        .name   = "2. Dump serial flash",
        .select = run_in_separate_task,
        .priv   = sf_dump_menu_task,
        .icon_type = IT_ACTION,
        .help   = "Reads the 8 MiB serial flash with Canon's READ routine.",
        .help2  = "-> ML/LOGS/SFDATA.BIN (about 1 minute)",
    },
    {
        .name   = "3. Session length",
        .priv   = &session_choice,
        .max    = 4,
        .choices = CHOICES("30 s", "1 min", "2 min", "5 min", "10 min"),
        .help   = "Length of the logging session (it can also be stopped from here).",
    },
    {
        .name   = "4. Start logging session",
        .select = session_toggle,
        .update = session_update,
        .icon_type = IT_ACTION,
        .help   = "DebugMsg + MPU + power-management log; close the menu, press keys.",
        .help2  = "-> ML/LOGS/DMnn.LOG, MPUnn.LOG, PMnn.LOG",
    },
    {
        .name   = "5. Dump LiveView buffers",
        .select = run_in_separate_task,
        .priv   = lv_dump_menu_task,
        .icon_type = IT_ACTION,
        .help   = "4 frames of the LV and HD YUV buffers (use it in LiveView).",
        .help2  = "-> ML/LOGS/LVn.422, HDn.422, LVINFO.TXT",
    },
    {
        .name   = "6. RAM dump label",
        .priv   = &ram_label,
        .max    = 4,
        .choices = CHOICES("LV", "MENU", "PLAY", "DOFF", "OTHR"),
        .help   = "State the camera will be in when the dump starts (file name).",
    },
    {
        .name   = "7. RAM dump delay",
        .priv   = &ram_delay,
        .max    = 3,
        .choices = CHOICES("5 s", "15 s", "30 s", "60 s"),
        .help   = "Countdown before the dump: time to bring the camera to that state.",
    },
    {
        .name   = "8. Start RAM dump",
        .select = run_in_separate_task,
        .priv   = ram_dump_menu_task,
        .icon_type = IT_ACTION,
        .help   = "256 MiB after the countdown; do not touch the camera while it writes.",
        .help2  = "-> ML/LOGS/RAM_<label>.BIN + .TXT (about 1 minute)",
    },
};

static void boot_grow(struct logbuf * lb, uint32_t size)
{
    char * big = 0;
    for (uint32_t s = size; s > lb->size && !big; s /= 2)
    {
        big = malloc(s);
        if (big) size = s;
    }
    if (!big) return;
    uint32_t old = cli();
    if (boot_active && lb->len < size)
    {
        memcpy(big, lb->buf, lb->len);
        lb->buf = big;
        lb->size = size;
        big = 0;
    }
    sei(old);
    if (big) free(big);
}

static void capture_init()
{
    boot_grow(&boot_dm, 2 * 1024 * 1024);
    boot_grow(&boot_mpu, 256 * 1024);
    boot_grown = 1;
    menu_add("Capture", capture_menu, COUNT(capture_menu));
    if (is_file("ML/CAPTEST.TXT"))
    {
        task_create("capture_selftest", 0x1e, 0x2000, capture_selftest_task, 0);
    }
}

INIT_FUNC("capture", capture_init);
