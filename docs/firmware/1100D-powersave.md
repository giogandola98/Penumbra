# Canon 1100D 1.0.5: `DisablePowerSave` / `EnablePowerSave` and the PowerMgr idle loop

Static analysis of the user's own ROM1 dump (16 MiB, offset 0 = 0xFF000000; data segment copied from
0xFF6DF858 to RAM 0x1900). Tags: **[V]** read directly in the disassembly, **[I]** inferred.
Answers `analysis/power-audit.md` §3.1 for the 1100D. EOSM (DIGIC 5) may differ and is **not** covered here.

## TL;DR

- `DisablePowerSave` / `EnablePowerSave` are a **reference counter** (`PowerMgr\PowerSaver.c`, max 32, asserts on
  under/overflow) that selects the **idle mode of Canon's `PowerMgr` task**. They do **not** touch auto power off
  (APO). APO is a separate mechanism (`Prohibit/PermitAutoPoweroff`, `PROP_ICU_AUTO_POWEROFF`) **[V]**.
- Counter > 0 → mode 1: plain WFI at full clock. Counter == 0 → mode 2 ("power save"): WFI at a lowered system clock,
  or a **deep "self-refresh" sleep** that gates all clocks (timers too) when the image engine is unclocked **[V]**.
- Canon's own boot does Disable (0xFF012454) … Enable (0xFF0141FC, after `startupInitializeComplete`), so stock idle
  ends in mode 2. Canon brackets ~60 operations with Disable/Enable pairs (LiveView, movie, USB/PTP, file jobs,
  display transitions, sensor cleaning, shutdown) **[V]**.
- ML's unconditional `call("DisablePowerSave")` (`src/init.c:478`, added in af31890bc, 2012) pins the counter at ≥ 1
  for the whole session: **the camera never uses the lowered-clock WFI or the deep sleep**. It does **not** block APO.
- It was added on purpose: in deep sleep the DryOS timers stop, so ML's `msleep`-driven tasks (intervalometer,
  motion detection, display-off features) froze with the display off. Older code used a Disable+Enable "kick"
  every 100 ms to postpone the deep sleep ("trick from AJ_MREQ_ISR … to prevent camera for entering deep sleep",
  f25d94833, 6ca4ac1de, e3ea2f5fb) **[V]**.

## Addresses

| What | Address | Notes |
|---|---|---|
| `DisablePowerSave` | 0xFF073008 | eventproc; table `{name, fn}` in data at ROM 0xFF6E0AC8 → RAM 0x2B68, registered by 0xFF0730E8 → 0xFF06D860 (register table) → 0xFF06D780 (register one eventproc) |
| `EnablePowerSave` | 0xFF073074 | same table |
| PowerSaver init | 0xFF072FB8 | called once at 0xFF012450 with (callback 0xFF01DB50, param 1); creates PowerMgr task, sets mode 1 |
| PowerSaver deinit | 0xFF072FEC | no callers found |
| "self-refresh happened?" (read & clear) | 0xFF0730E4 → 0xFF1EA95C | callers 0xFF01C194, 0xFF027848, 0xFF0BA784 then call `LoadCalendarFromRTC` (0xFF0694E4) to resync time |
| PowerSaver state | RAM 0x2B60 | +0 init magic (= ptr to "PowerSaver"), **+4 counter** |
| PowerMgr state | RAM 0x7504 | +0 running, +4 param (1), **+8 mode** (0/1/2), +0xC "not in self-refresh" flag, +0x10 callback, +0x14 last-sleep timestamp |
| set mode | 0xFF1EA920 | mode 2 also stamps +0x14 with "now" |
| PowerMgr task entry | 0xFF1EA7D4 | `task_create("PowerMgr", prio 0x20, stack 0x400)` at 0xFF1EA8D4 |
| idle hot spot | 0xFF1EA818 | `ldr` of 0xC0400008 in the mode-2 branch |
| WFI helper (`waitForInt` eventproc) | 0xFF1EA99C | IRQ/FIQ masked, `mcr p15,0,r0,c15,c8,2` (ARM946 WFI) at 0xFF1EA9B0 |
| `selfRefresh` eventproc / callback | 0xFF01DB50 | registered by 0xFF1EA970 |
| time source | 0xFF076138 | reads DryOS timer-service time at RAM 0x2C08 (ms **[I]**) |
| ClockSaver (`Pwrcnt.c`) | ~0xFF1D6DE8 | owns LCLK: sets/clears 0xC0400008 bit 1 depending on client count |

## Pseudo-code (own words)

```
Disable():  if initialised { irq_off; set_mode(1); assert(cnt < 32); cnt++; log; irq_on }
Enable():   if initialised { irq_off; assert(cnt > 0); if (--cnt == 0) set_mode(2); log; irq_on }

PowerMgr task (prio 0x20, runs whenever nothing more important is ready):
  forever:
    irq_off
    switch mode:
      0: nothing                                   # busy loop
      1: WFI                                       # what ML forces
      2: now = time()
         if CLOCK_ENABLE(0xC0400008) bit1 (image-engine LCLK on):
            if param && LCLK_MASK(0xC0F01004) == 0x400:
               CLKSEL(0xC0400004) low byte 0x03 -> 0x17 ; WFI ; back to 0x03
               last = now
            # else: no WFI at all, loop again (busy poll)
         else (LCLK off):
            if now - last > 1000: last = now; flag=0; selfRefresh(); flag=1
            # else busy poll until the 1000-tick grace expires
    irq_on
```

`selfRefresh` (0xFF01DB50), only if GPIO 0xC0220094 bit0 is set:
mask IRQ/FIQ → write 1 to 0xC0201308 → clean+invalidate and disable the D-cache → clear 0xC0210600 bit0 and wait
for it to read 0 → set 0xC0100204 bit27, wait for 0xC0100110 bit31 → **0xC0400008 = 0 (all module clocks off,
timers included)**, clear 0xC0400004 bits 0-1 (slow clock), clear 0xC022F0B4 bit0 → **poll 0xC0201308 bit4 (wake)**
→ restore in reverse (0xC022F0B4 bit0, clock select |= 3, a handshake on 0xC022F188/18C/198/19C with 0xABC and
polls of 0xC022F198 bit0, restore 0xC0400008, clear 0xC0100204 bit27, set 0xC0210600 bit0, D-cache on,
0xC0201308 = 0). Name and register roles (DRAM self-refresh, PLL) are **[I]**; the access sequence is **[V]**.

Notes:
- The counter is properly nested; Disable always forces mode 1, only the last Enable returns to mode 2.
- `set_mode(2)` restamps `last`, so every Enable postpones the deep sleep by ≥ 1000 ticks. That is why the old ML
  Disable+Enable "kick" worked.
- In mode 2 the loop **busy-polls** whenever neither sleep condition holds. Mode 2 is not uniformly cheaper than
  mode 1; it depends on the LCLK mask being exactly 0x400 at idle on real hardware **[I]**.

## Who calls it in Canon firmware (60 Disable / 65 Enable sites)

Held for the duration of an activity **[V]** (Disable site → Enable site):
- LiveView/EVF: `evfActive` 0xFF0F7C80 → `evfStandby` 0xFF0F99F8. So **in LiveView Canon itself uses mode 1**.
- Movie: `mvrRecStart` 0xFF17B4FC → `mvrRecStopped` 0xFF17D204.
- USB/PTP sessions, PTP UI lock (0xFF11BEAC/0xFF11BFC4, 0xFF138930…, 0xFF13991C/0xFF139964).
- File manager and FR/FW sync read/write jobs, card mount, capture jobs (`CreateJob`/`DeleteJob`), DirectPrint.
- Display on/off and HDMI connection (only during the transition), sensor cleaning (`GUIActiveSweep`), shutdown,
  SIO/MREQ (MPU link) at 0xFF1CAF74/0xFF1CAF78, SD-COM, tune-data dump (`CaptureCmd.c`), boot.

There is no permanent Disable at idle on the shooting screen or in playback. Only ML holds one.

## ML side

- `src/init.c:478` (boot, never released) and `src/installer.c:466` (installer only).
- History: f25d94833 / 6ca4ac1de / e3ea2f5fb used periodic Disable+Enable kicks so that `msleep` and the
  intervalometer kept running with the display off. af31890bc replaced the kicks with one permanent Disable at boot.
- Nothing else in `src/` or `modules/` calls these eventprocs. Code that depends on timers running with the LCD off
  (and so relies on the boot Disable): intervalometer / bulb ramping (`shoot.c`), motion detection and audio
  remote, `powersave.c` LCD-off/dim timers, the idle LED blink, Lua background scripts, `clock_task`.
- `MEM(0xC0400008) & 2` (LCLK) is already used by ML as an "engio safe to write" guard (`src/debug.c:2028`).

## Answer

- **Does ML's call block APO?** No (high confidence). The eventprocs only change the PowerMgr idle mode.
- **Does it block deep idle?** Yes (high confidence). With ML the idle loop is always a full-clock WFI. The
  reduced-clock WFI (LCD on, LCLK mask 0x400) and the clock-gated self-refresh sleep (image engine off) never happen.
- **Battery impact:** expected small with the LCD on (full- vs reduced-clock WFI). Larger with the display off
  before APO fires. LiveView and movie are unaffected, since Canon forces mode 1 there anyway **[I]**.

## Recommendation for ML

Do not just delete the call. Replace it with a hold that is taken only when needed:
1. Add a small ref-counted wrapper (`powersave_hold()` / `powersave_release()` → `call("DisablePowerSave")` /
   `call("EnablePowerSave")`), never unbalanced. Canon asserts on underflow.
2. Take the hold only while an ML feature needs timers with the image engine off: intervalometer / bulb ramping /
   timelapse running, motion detect, audio remote shot, LCD-off powersave timers, a background Lua script, file I/O
   that ML drives itself. Release it when the feature stops.
3. At boot, drop the unconditional Disable (or call Enable once after `call_init_funcs`, keeping symmetry).
4. Expect ML's µs clock and `msleep` to jump after a deep sleep. Anything time-based must tolerate that, as
   Canon's own `LoadCalendarFromRTC` resync does.

## Verification plan

QEMU first (`ci/qemu`, 1100D):
1. Fix the emulation gap. Model the PM registers the mode-2 path reads (qemu-eos already stores 0xC0400008 as
   `CLOCK_ENABLE` and 0xC0F01000-0xC0F010FF as plain RAM in the "Power" handler):
   - `0xC0400008` bit1 (LCLK): reflect what ClockSaver writes. Check the `-d io` log for its idle value.
   - `0xC0F01004`: per-module LCLK mask (ClockSaver logs 0xC0F01000/04 as `NRES`/`LCLK`) **[I]**; the low-clock WFI path needs `== 0x400`. Store writes and log them.
   - `0xC0400004` low byte 0x03 ↔ 0x17: store only.
   - self-refresh: `0xC0220094` bit0 gate (GPIO), `0xC0210600` bit0 → 0, `0xC0100110` bit31 → 1,
     `0xC022F198` bit0 → 0 (all "ready" immediately). `0xC0201308` bit4 = wake: set it when an interrupt is pending,
     and count the poll loop as halted time in `plugins/wakeups.c`.
   - Also treat the busy-poll at 0xFF1EA7E0-0xFF1EA8D0 in mode 2 as "not idle" in reports, so the Canon baseline is
     honest.
2. Observe in the DryosDebugMsg log: `[PM] DisablePowerSave/EnablePowerSave (Counter = n)` and
   `[PM] pmSelfRefresh : In/Out`. Watch RAM 0x2B64 (counter) and 0x750C (mode).
   Expected: Canon alone → counter 0, mode 2. Current ML → counter ≥ 1, mode 1. Patched ML → counter 0 at idle,
   > 0 while the intervalometer runs.
3. Regression: with the patch, run the intervalometer with the display off and check that ML tasks keep their wake
   rate (no stalls) while the hold is taken.

Hardware last (only when the user says so): measure battery current or runtime with the LCD on at idle and with the
LCD off, stock vs current ML vs patched ML. Check that intervalometer shots are on time with the display off, and
that APO still fires at the configured time.
