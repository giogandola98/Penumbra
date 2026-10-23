# Known problems and limits

This page lists what Penumbra does not do yet, what it cannot do, and where it behaves differently from Magic
Lantern. It is kept up to date with the branches listed in [ARCHITECTURE.md](ARCHITECTURE.md#4-where-we-are).
Items marked *(branch)* are true only once the named branch is merged.

**Penumbra has not run on a real camera. Do not install it on a camera yet.**

## Current problems (fixable)

- **No EOS M emulation yet.** All emulation results come from the Canon 1100D (DIGIC 4). The EOS M (DIGIC 5)
  scenarios are skipped until a ROM dump of the EOS M is available.
- **EOS M power management has not been reverse engineered.** The power-save hold design comes from the 1100D
  firmware. If the EOS M's `PowerMgr` works differently, the photo profile behaves differently too. Until that
  analysis is done, the holds can be switched off at build time (`POWERSAVE_HOLD=n`) *(branch
  `power/powersave-hold`)*.
- **Deep sleep is not verified.** The emulator does not model the power-management hardware yet, so no test has
  shown the camera entering self-refresh sleep with Penumbra loaded and waking up correctly.
- **ML's clocks lag after a deep sleep.** The DIGIC timer stops during self-refresh, and ML does not resync
  from the real-time clock as Canon does. Anything that measures elapsed time across an idle period with no hold
  (idle LED "30 s since the last key", powersave countdowns, config autosave) sees less time than really
  passed. Features that hold power save are not affected.
- **The low-power idle is not always cheaper.** On the 1100D, Canon's low-power idle loop busy-polls instead of
  waiting when the image engine clock is on but its clock mask is not the expected value. Battery gains must be
  measured on hardware before they are claimed.
- **Remaining wakeups.** After the low-risk fixes, the estimated ~103 wakeups/s at LiveView idle are mostly
  the overlay task (~60/s, two wakes per frame), `shoot_task` (20/s), `cls_task` (10/s), `clock_task` (5/s) and
  the histogram task (5/s). These are the event-core migration steps in [ARCHITECTURE.md](ARCHITECTURE.md).
- **The default RAW histogram keeps LiveView in raw mode** for RAW shooters, which copies every raw frame. A
  YUV default or an on-demand snapshot is planned.
- **Intervalometer power.** With default settings, LiveView and the display stay on between timelapse shots,
  and the status text is still drawn with the display off. The 10 ms spin in the last second before a shot is
  removed *(branch `power/low-risk`)*. Pausing LiveView between shots is not done yet.
- **Focus-ring APO prolong.** In manual focus, any change of the lens focus position postpones auto power off.
  A focus-by-wire lens whose position jitters could keep the camera on. A debounce is planned.
- **Card I/O during deep sleep.** Canon wraps its own card access in a power-save hold. Upstream ML's file
  calls do not, so a deep sleep could start in the middle of an ML transfer and freeze it until the next wake.
  Being fixed: every ML file operation takes a hold for the duration of the call *(branch `power/sd-safe`, in
  progress)*.
- **Long-lived files.** A background Lua script that never closes or flushes its files keeps unwritten data
  in RAM across idle periods. Scripts should call `:close()` or `:flush()`.
- **"Turn off LCD" (LiveView pause) has never been validated on the EOS M** (inherited from upstream).
- **The idle LED blink** is on by default while the display is off. With holds it stops during deep sleep.

## Emulation limits

- **No power model.** QEMU counts events (wakeups, interrupts, task switches, power-save counter changes). These
  are relative proxies, compared with a Canon-only run and an upstream-equivalent run on the same runner. They
  are never battery estimates.
- **Canon's idle does not sleep in emulation.** With the power-save counter at 0, the 1100D idle loop polls
  power-management registers that QEMU does not emulate, so it spins. For that reason, "WFI/s" and "busy
  ratio" cannot be compared between the Canon-only run and an ML run that holds the counter. Task switches/s and
  IRQ/s can.
- **No LiveView.** The sensor path is not emulated. Overlay and per-frame costs come from host benchmarks
  (instruction counts on an emulated ARM926 with synthetic frames) and from estimates.
- **Partial peripherals.** MPU, display and SD are emulated in part. SD commands complete instantly, so a
  transfer is never in flight when the idle task runs. Modelling that is part of the SD-safe work.
- **ROM0 is not mapped for the 1100D.** The harness pre-creates ML's ROM backup files so that ML skips the
  backup on first boot.
- **Timing noise.** Guest time follows host time, so a loaded CI runner can shift rates a little. Small
  differences need two runs.
- **Different body.** The 1100D validates the toolchain, the harness and the hold logic. It is a DIGIC 4
  camera with a mirror and an optical viewfinder. The EOS M is DIGIC 5 and always in LiveView.

## Fundamental limits

- **The sensor and the display dominate the EOS M's power use in LiveView.** Penumbra cannot turn them off
  while you are shooting. It can only stop the waste that ML adds: CPU wakeups, redundant overlay work, a raw
  LiveView stream nobody needs, and blocked low-power idle.
- **The biggest gains are expected when the camera is idle or the display is off:** outside LiveView, in
  playback, during a timelapse with the display off, or before auto power off. While you compose in LiveView,
  the difference will be small.
- **No battery percentage or shots-per-charge claims** will be made until a hardware A/B measurement exists
  (stock firmware, upstream ML and Penumbra, same camera, same protocol).

## Behaviour changes compared with Magic Lantern

These come from the power work. The `full` profile keeps upstream behaviour unless noted.

| Change | Detail | Profile / branch |
|---|---|---|
| Config autosave timing | A setting changed while the ML menu is closed (Lua `menu_set`, quick-access items) is saved at the next menu close or at shutdown, not within 0.5 s. A battery pull in between loses it. | photo, `power/low-risk` |
| Crash log latency | Crash and assert logs are written within about 1.2 s instead of 0.2 s. If the camera locks up in that second, the log can be lost. | photo, `power/low-risk` |
| Display-change latency | State changes not caused by a key (Canon leaving image review on a timer, the bottom bar after an exposure change, warnings) are noticed within 500 ms instead of 50 ms. Key presses are still handled at once. | both, `power/low-risk` |
| Lens info outside LiveView | Focal length, focus distance and DOF are refreshed outside LiveView only while someone uses them: the ML menu, the info screen, a running feature, or a Lua read in the last 10 s. A script's first `lens.*` read after a quiet period, or a `property.LV_LENS` handler, can see a stale value. | both, `power/lens-poll` |
| Timers stop in deep sleep | With no hold, the clock task, overlays, NotifyBox timeouts and crash-log polling stop during deep sleep and resume on the next wake. A NotifyBox can stay on a dark screen until then. | photo, `power/powersave-hold` |
| Removed features | Video, audio and developer features and modules are not in the photo build. See [BUILD-PROFILES.md](BUILD-PROFILES.md). | photo |
| Movie menu remnant | The Movie tab is still present in the photo build, with only the entries that were not removed. | photo |
| Trap focus | Not available on the EOS M (disabled upstream too). | both |
| `sd_clock` | Shipped, off by default as upstream. When enabled, it holds power save, so the camera never deep-sleeps, until it is known whether the SD overclock survives the sleep. | `power/sd-safe` |

### The hold risk

Holds are what let Penumbra drop the permanent `DisablePowerSave`. They also add a new kind of bug: **a
feature that runs from timers but takes no hold freezes when the display is off, until the next key press or
other wake event.** The list of holders comes from a code audit of `src/`, the modules shipped in the photo
build, and Lua. Modules that the photo build does not ship were only checked for "LiveView only" behaviour.
If a feature stalls with the display off, a missing hold is the first suspect. The Debug menu entry
*Powersave holds* shows the current holders.

## Upstream divergence

Penumbra is a fork of `magiclantern_simplified` (`dev`). Most changes are behind `CONFIG_PROFILE_PHOTO` or a
build switch, so `ML_PROFILE=full` stays close to upstream and serves as the reference build. Some fixes apply
to both profiles: the `tweak_task` rate and the lens-info poll. Merging new upstream work needs care around
the files Penumbra touches most: `zebra.c`, `shoot.c`, `powersave.c`, `menu.c`, `tweaks.c`, `lens.c` and
`fio-ml.c`. Penumbra's changes are not submitted upstream, and Magic Lantern's support channels do not cover
Penumbra.
