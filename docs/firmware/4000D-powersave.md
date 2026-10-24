# Canon 4000D 1.1.0: PowerMgr idle loop and the selfRefresh deep sleep

Static analysis of the maintainer's own ROM1 dump (offset 0 = 0xFE000000; DryOS kernel and data copied to RAM
0x1900). Companion of `1100D-powersave.md`, which explains the mechanism; this page records where the 4000D
differs. Tags: **[V]** read in the disassembly, **[I]** inferred. Descriptions are ours.

## TL;DR

- Same design as the 1100D: `DisablePowerSave` / `EnablePowerSave` are a nested counter (max 32) that selects the
  idle mode of the `PowerMgr` task. Counter > 0: plain WFI. Counter 0 (mode 2): a low-clock WFI while the
  image-engine clock (LCLK, `0xC0400008` bit 1) is on, or the clock-gated `selfRefresh` sleep once LCLK is off and
  nothing restamped PowerMgr for more than 1000 ticks **[V]**.
- The deep sleep itself differs: the 4000D **waits in a WFI with interrupts masked** instead of polling a wake-flag
  register (1100D: `0xC0201308` bit 4). The pending interrupt (key / MPU) ends the WFI; the IRQ is taken when
  `selfRefresh` restores CPSR on the way out **[V]**.
- Other register differences: the enable gate is GPIO `0xC0220018` bit 0 (1100D `0xC0220094`); the low-clock WFI
  brackets `0xC0400044/48` and `0xC0400030` (1100D: clock select `0xC0400004` 0x03 ↔ 0x17); extra handshakes on
  `0xC0100300/0x720/0x724` and `0xC022F500/50C/514/528` **[V]**.
- For emulation only the clock gate was missing: everything `selfRefresh` polls was already answered by qemu-eos'
  generic handlers, so without the gate the DryOS tick ended each sleep within 10 ms and PowerMgr re-entered
  `selfRefresh` about once a second (QEMU-EOS-Optimized, `hw/eos/cams/pm_4000d.c`).

## Addresses

| What | 4000D 1.1.0 | 1100D 1.0.5 | Notes |
|---|---|---|---|
| `DisablePowerSave` / `EnablePowerSave` | 0xFE11A288 / 0xFE11A2F4 | 0xFF073008 / 0xFF073074 | eventprocs, `./PowerMgr/PowerSaver.c` asserts |
| PowerSaver state | RAM 0x31648 (+4 counter = 0x3164C) | RAM 0x2B60 (0x2B64) | `4000D-consts.md` |
| PowerSaver init | 0xFE11A254, called from 0xFE11F190 with (callback 0xFE0C9618, param 1) | 0xFF072FB8 | creates PowerMgr, mode 1 |
| PowerMgr state | RAM 0x39650: +0 running, +4 param, +8 mode, +0xC "after self-refresh" flag, +0x10 callback, +0x14 last stamp | RAM 0x7504 | |
| PowerMgr start | 0xFE289650 (`task_create("PowerMgr", 0x20, 0x400, 0xFE289544)`) | 0xFF1EA7D4 | |
| PowerMgr task loop | 0xFE289544 | 0xFF1EA7D4 | IRQs off around each pass (RAM 0x3000 / 0x3004) |
| set mode | 0xFE289684 (mode 2 stamps +0x14 via the time getter 0xFE11E920) | 0xFF1EA920 | |
| read & clear "a self-refresh happened" | 0xFE2896C0 (thunk 0xFE11A364) | 0xFF1EA95C | callers 0xFE0C7B58 (`GUI_STATE`), 0xFE0CE044, 0xFE0D303C (`GetCurrentDcsParam`) then re-read the RTC (0xFE112F34, next to `LoadCalendarFromRTC` 0xFE112DA8) and set the time |
| selfRefresh | 0xFE0C02E4 (callback 0xFE0C9618 is a branch to it) | 0xFF01DB50 | below |
| ClockSaver (`[CLKSAVER] ClockSaverExecute`) | 0xFE27A2C0; add client 0xFE27A3C4, remove 0xFE27A470 | ~0xFF1D6DE8 | 30 client counters at RAM 0x611A8; sets `0xC0400008` bit 1 while any client is counted, clears it at zero; writes the client masks `0xC0F01000/04` |

## PowerMgr loop (own words)

```
forever:
  wait until running; irq_off
  mode 1: WFI
  mode 2: now = tick
          if CLOCK_ENABLE(0xC0400008) bit1:              # LCLK on
             if param && LCLK_MASK(0xC0F01004) == 0x400:
                0xC0400030 rewritten; 0xC0400048 = 5; 0xC0400044 = 5
                WFI
                0xC0400048 = 0; 0xC0400044 = 0; 0xC0400030 bit0 cleared
             last = now                                # also when the mask differs (busy pass)
          else if now - last > 1000:
             last = now; log "[PM] pmSelfRefresh : In"; flag = 0
             selfRefresh(); flag = 1; log "[PM] pmSelfRefresh : Out"
  irq_on
```

Difference from the 1100D: with LCLK on and the mask not 0x400 the 4000D still restamps `last`; the 1100D does
not **[V]**. Either way the deep sleep needs LCLK off for more than 1000 ticks.

## selfRefresh (0xFE0C02E4)

Only if `0xC0220018` bit 0 is set; otherwise it returns at once **[V]**.

1. Mask IRQ and FIQ (previous I/F bits kept). Clean and invalidate the whole D-cache by set/way, disable it (CP15
   control bit 2).
2. `0xC0100204` bit 24 cleared; low half of `0xC0100300` saved in RAM 0x4D0A8 and cleared; `0xC0100724` bit 31
   cleared; `0xC0100204` bit 27 set, then poll `0xC0100110` until bit 31 is set (DRAM self-refresh request /
   acknowledge **[I]**).
3. Save `CLOCK_ENABLE` (`0xC0400008`) and write 0: every module clock off, DIGIC timers included.
4. `0xC022F500` = 1, `0xC022F50C` = 0x10F1D; `0xC0400008` bit 0 cleared (already 0), clock select `0xC0400004`
   bits 0-1 cleared; short delay; `0xC04000B4` = 0; delay.
5. **WFI** (`mcr p15,0,r0,c15,c8,2`) with IRQ/FIQ masked: the core sleeps until an interrupt is asserted.
6. Wake path: `0xC04000B4` = 3, clock select bits 0-1 set, `CLOCK_ENABLE` bit 0 set; `0xC022F50C` = 0,
   `0xC022F500` = 0; `0xC022F17C` = 0x11, 0x19; `0xC022F198` = 0, `0xC022F19C` = 0x1B6C, `0xC022F198` = 2,
   `0xC022F17C` = 0x1D, `0xC022F514` = 3 then 0; poll `0xC022F198` until bit 0 is set; `0xC022F17C` = 0x1F,
   `0xC022F528` = 1 then 0 (PLL / DRAM controller restart **[I]**).
7. Write the saved `CLOCK_ENABLE` back; `0xC0100204` bit 27 cleared; `0xC0100720` = 0x80000000; second
   `0xC022F198`/`0xC022F19C` (= 0x2D) handshake with the same poll; restore `0xC0100300` from RAM 0x4D0A8,
   `0xC0100724` bit 31 and `0xC0100204` bit 24 set; D-cache on; CPSR I/F restored, so the waking interrupt is
   serviced now.

Compared with the 1100D sequence (`1100D-powersave.md`): same CLOCK_ENABLE save / 0 / restore and the same
`0xC0100204` bit 27 / `0xC0100110` bit 31 handshake; no `0xC0201308` (neither the "1" written on entry nor the
bit 4 wake poll), no `0xC0210600`, no `0xC022F0B4`; the post-wake handshake moved from `0xC022F188/18C` with 0xABC
to `0xC022F17C/19C/5xx`. The card power and SD controller registers are not touched, as on the 1100D **[V]**.

## What the emulator needs (QEMU-EOS-Optimized)

| Access | qemu-eos before | Needed |
|---|---|---|
| `0xC0220018` bit 0 read | GPIO default 1 | ok |
| `0xC0100110` bit 31 poll | 0x80000000 (`eos_handle_basic`, added for the 1300D, whose ROM has the same code) | ok |
| `0xC022F198` bit 0 polls | toggles 0/1 | ok |
| WFI with IRQ masked | ARM946 WFI halts the vCPU until an interrupt is pending, mask or not | ok |
| LCLK off with the display off | JPEG core reports ready on the 4000D (as on the 1300D), so ClockSaver drops its last client | ok |
| `CLOCK_ENABLE = 0` | stored only: the DryOS tick kept running and ended the WFI within 10 ms | **clock gate**: timers, tick and HPTimers stop from that write until the next non-zero write (`hw/eos/cams/pm_4000d.c`) |

Result in emulation: one sleep for the whole display-off period, ended by the INFO key through the MPU interrupt
0x50 (`docs/analysis/powersave-hold.md`, 4000D section). Not covered: real time across the sleep (RTC drift),
APO and LCD auto-off paths, and whether `0xC0F01004` really reads 0x400 at idle on the camera.
