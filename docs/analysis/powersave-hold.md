# Counted powersave holds instead of ML's permanent `DisablePowerSave`

Status: implemented, builds and host tests green. **Not yet run in QEMU with the PM registers modelled, nor on
hardware.** Spec: `docs/firmware/1100D-powersave.md` (RE of 1100D 1.0.5). Branch `power/powersave-hold`.

## Problem

Canon's `DisablePowerSave` / `EnablePowerSave` eventprocs are a nested counter (max 32, asserts on underflow) that
selects the idle mode of the `PowerMgr` task. Counter > 0: plain WFI at full clock. Counter 0: WFI at a lowered clock,
or (image engine unclocked, ≥ 1000 ticks after the last stamp) a self-refresh sleep that gates **all** clocks,
DryOS timers included, until a wake event (key / MPU). Canon brackets activities (LiveView, movie, USB, file jobs,
display transitions, boot) with Disable/Enable pairs and idles at counter 0.

Upstream ML calls `call("DisablePowerSave")` once in `my_big_init_task` and never re-enables it (af31890bc, 2012):
msleep-driven features froze in the deep sleep. The camera therefore never uses the low-clock WFI or the deep sleep.
APO is a separate mechanism and is not affected either way.

## Design

`src/powersave_hold.h` (pure C, header-only so the host test runs the same code) keeps the bookkeeping;
`src/powersave.c` wraps it for DryOS.

```c
int  powersave_hold(const char * who);      /* nested per name; 0 ok, -1 refused   */
int  powersave_release(const char * who);   /* drops only a hold `who` owns        */
void powersave_hold_set(const char * who, int on);  /* level-triggered, cheap to poll */
int  powersave_holders(void);               /* total ML holds (debug / tests)      */
int  powersave_hold_count(const char * who);
void powersave_hold_init(void);             /* ML init, before the first hold      */
```

- **Per-name accounting.** Up to 8 names are tracked (compared by content: a module's literal and the core's differ
  in address). A release by a name that holds nothing is refused, so ML's counts never go negative and one buggy
  holder cannot drop another's hold. If the table is full the hold is still counted (unnamed) so the camera stays
  awake; a per-name nest limit of 255 catches leaks.
- **Canon sees at most one hold from ML.** Only the total's 0→1 edge calls `DisablePowerSave` and only 1→0 calls
  `EnablePowerSave`; a `ps_canon_disabled` flag additionally guarantees Enable is never called more times than
  Disable. Canon's counter thus never underflows because of ML and ML never adds more than 1 to it.
- **Thread safety.** A DryOS semaphore (`ps_hold`, created by `powersave_hold_init()` in `my_big_init_task`
  before any ML task or module exists) serialises the bookkeeping **and** the Canon call, so the eventprocs are
  invoked in the same order as the edges. The API is therefore **task context only** (no interrupts, no timer
  callbacks). Property handlers, CBRs and menu callbacks run in tasks and are fine. `powersave_hold_set()` does an
  unlocked pre-check (only its caller changes that name's count), so loops can call it every iteration for free.
- **Logging.** Every hold/release logs `[PS] hold|release <who> -> <total>` through `DryosDebugMsg`, next to
  Canon's own `[PM] DisablePowerSave/EnablePowerSave (Counter = n)`. QEMU `-d debugmsg` shows both.
- **Debug menu.** Debug → *Powersave holds* shows the total and, in its help line, the holders (`menu, lua task x2`).
- **Modules.** All functions are global, so they land in `magiclantern.sym` / `<cam>_<fw>.sym` and modules resolve
  them at load time (verified: `lua.mo`, `file_man.mo` import `powersave_hold`, `powersave_release`,
  `powersave_hold_set`). The API exists in **every** build, with or without `CONFIG_POWERSAVE_HOLD`, so a module
  loads on all of them.

### Build switch: `CONFIG_POWERSAVE_HOLD`

| Build | `CONFIG_POWERSAVE_HOLD` | Boot | Holds |
|---|---|---|---|
| EOSM `ML_PROFILE=photo` (default) | on (`POWERSAVE_HOLD ?= y` in `platform/EOSM.202/Makefile`) | boot hold, released when init completes | forwarded to Canon |
| EOSM `ML_PROFILE=full` | off | upstream permanent `DisablePowerSave` | counted only |
| any camera, e.g. 1100D | off by default; `make POWERSAVE_HOLD=y` turns it on | as above | as above |

`platform/Makefile` adds `-DCONFIG_POWERSAVE_HOLD` when `POWERSAVE_HOLD=y`. Opt out of the photo default with
`make POWERSAVE_HOLD=n`. Always `make clean` when switching (it is a compiler flag, like the profile).

**Why not in `full`:** `full` is the upstream-equivalent reference build used as the comparison baseline
(`docs/BUILD-PROFILES.md`), and the change alters idle behaviour for every feature, including video and audio paths
(raw video, audio meters) that the photo profile drops and that were not audited for timer dependence. It is not
"trivially safe", so `full` keeps the permanent disable. It still counts holds, so the Debug menu entry works there.

### Boot

`my_big_init_task` (`src/init.c:485-489`): `powersave_hold_init()`, then (only without the switch) the upstream
`call("DisablePowerSave")`, then `powersave_hold("boot")`. The hold covers CBR/menu/module init, config load and
task creation and is released right after `ml_started = 1` (`src/init.c:546`), mirroring Canon's own boot
Disable…Enable around `startupInitializeComplete`. Canon holds its own boot pair, so ML's release never drops
Canon below its own count.

## Holders

Rule: hold only while a feature must keep running **from timers with nobody touching the camera**. Anything a key,
the shutter or an MPU message drives wakes the CPU by itself; LiveView and movie recording are held by Canon itself
(`evfActive`/`mvrRecStart`).

| Name | Where | Held while | Kind |
|---|---|---|---|
| `boot` | `src/init.c:489`, `:546` | ML init (until `ml_started`) | pair |
| `rom backup` | `src/init.c:254-257` | first-boot ROM backup task | pair |
| `intervalometer` | `src/shoot.c:5487` (top of `shoot_task` loop) | `intervalometer_running` (incl. countdown, start delay, auto-start at boot); covers `adv_int`, `deflick`, silent-picture timelapse, Lua `interval` | level |
| `motion detect` | `src/shoot.c:5491` | motion detection enabled **in LiveView** (outside LV it only runs while half-shutter is held) | level |
| `audio remote` | `src/shoot.c:5494` | audio remote shot armed (`FEATURE_AUDIO_REMOTE_SHOT`; compiled out of EOSM photo and 1100D) | level |
| `lcd remote` | `src/shoot.c:5497` | LCD-sensor remote armed (`FEATURE_LCD_SENSOR_REMOTE`, cameras with the sensor) | level |
| `bulb` | `src/shoot.c:2730-2878` (`bulb_take_pic`) | any ML-timed bulb exposure (bulb timer, intervalometer/bulb ramping, HDR bulb, Lua) | pair |
| `bulb timer` | `src/shoot.c:5656-5681` | the 2 s countdown after half-shutter release, until the exposure ends or is cancelled | pair |
| `hdr shot` | `src/shoot.c:4941-4967` (`hdr_shot`) | HDR/bracketing sequences (after a shot, from the intervalometer, or from remote triggers) | pair |
| `lv pause` / `lv resume` | `src/powersave.c:179-188`, `:204-218` | ML's LiveView pause/resume transitions ("Turn off LCD") | pair |
| `menu` | `src/menu.c:5700`, `:5721` | ML menu open (key repeat, live values) | level |
| `menu action` | `src/menu.c:7208-7211` (`run_in_separate_task`) | actions started from the menu (focus stacking, module actions…) after the menu closed | pair |
| `screenshot` | `src/debug.c:646` | 10 s screenshot countdown | level |
| `play timelapse` | `src/tweaks.c:1002` | timelapse playback in PLAY mode (compiled out of EOSM) | level |
| `lua script` | `modules/lua/lua.c:800`, `:894` | a script's main body runs (load / simple script) | pair |
| `lua task` | `modules/lua/lua_task.c:55`, `:68` | every `task.create` task of a script, for its whole life | pair |
| `lua events` | `modules/lua/lua.c:302-304`, `:358` | any loaded script handles the periodic `shoot_task` or `seconds_clock` event | level |
| `file copy` | `modules/file_man/file_man.c:519`, `:575` | file manager copy/move | pair |

"Level" holders are re-evaluated each time their task runs (`powersave_hold_set`); "pair" holders bracket the
activity.

### Deliberately not held (behaviour change vs upstream)

- **Idle LED blink** (`idle_led_blink_step`, every 5 s with the display off): runs from `cls_task` timers. In deep
  sleep it stops. That is the point of the sleep; the LED blink was a reminder that the camera is drawing power.
- **ML's display-off state itself** (LV paused by "Turn off LCD"): only the transitions are held. Waking up is
  key-driven (`handle_buttons` → `idle_wakeup_reset_counters`, then `cls_task` resumes LV once the key interrupt
  restarted the clocks).
- **`clock_task`, overlays, NotifyBox timeouts, crash-log polling, `debug_task`**: freeze with the CPU and resume on
  the next wake. A NotifyBox can stay on a (dark) screen until then.
- **Raw video (`mlv_rec`, `mlv_lite`)** and other LV-only features (zebras, focus peaking, ETTR in LV, silent
  pictures, focus stacking in LV, trap focus in LV): Canon holds in LiveView.

## Risks

1. **Unknown semantics on EOSM (DIGIC 5).** The RE covers only the 1100D. If EOSM's PowerMgr differs, the photo
   profile behaviour differs too. Disassemble EOSM 2.0.2 `DisablePowerSave`/`EnablePowerSave`/`PowerMgr` before
   hardware; until then the switch is easy to turn off (`POWERSAVE_HOLD=n`).
2. **A missed holder freezes a feature** with the display off until the next key press. The holder list is from a
   code audit (`msleep`/timer loops in `src/`, the shipped photo modules and Lua). Modules not in the photo zip
   (e.g. `raw_video`, `bench`, `selftest`) were not audited beyond "LV only".
3. **ML's clocks lag after a deep sleep.** The DIGIC timer stops, so `get_ms_clock()`/`get_seconds_clock()` do not
   count the sleep (Canon resyncs its calendar from the RTC; ML does not). Anything that compares ML time across an
   unheld idle period (idle LED "30 s since last key", powersave countdowns, config autosave) sees less elapsed time
   than real. Held features are unaffected.
4. **Mode 2 is not always cheaper.** With LCLK on but the LCLK mask ≠ 0x400, Canon's mode-2 loop busy-polls
   instead of WFI (`1100D-powersave.md`). On real hardware Canon idles like this anyway; measure before claiming a
   battery win.
5. **Level holders lag their flag by up to one loop** (50–200 ms for `shoot_task`). Harmless in practice: every
   `EnablePowerSave` restamps Canon's "last" time, so the deep sleep cannot start for ≥ 1000 ticks after ML's last
   release, and flags set from other tasks are set while a held activity (menu, menu action) is still running.
6. **Task context only.** Calling the API from an interrupt or a timer callback would block on the semaphore.
   Nothing does today; the header says so.
7. **Unbalanced module code.** A module that releases more than it held is refused (logged `REFUSED`); one that
   leaks keeps the camera at mode 1 (= upstream behaviour). Neither crashes Canon.

## Verification

Done:

- `make -C tests/host`: `test_powersave_hold` checks nesting, refusal of unmatched/double/NULL releases, name
  matching by content, the level-triggered form, table overflow, the nest limit, and 200 000 random operations
  against a reference model, with a fake Canon counter that must stay in {0, 1} and equal `total > 0` after every
  step. A mutation (dropping the overflow guard) is caught.
- Builds with `FATAL_WARNINGS=y`: EOSM.202 photo, photo `POWERSAVE_HOLD=n`, full; 1100D.105 default and
  `POWERSAVE_HOLD=y`.

| autoexec.bin | before | after | Δ |
|---|---:|---:|---:|
| EOSM.202 photo (hold on) | 425,856 | 428,000 | +2,144 |
| EOSM.202 full | 457,792 | 460,000 | +2,208 |
| 1100D.105 (default / `POWERSAVE_HOLD=y`) | 447,584 | 449,664 | +2,080 |

About 1.9 KB of it is `powersave.o` (bookkeeping, semaphore wrapper, Debug-menu entry, log strings).

### First QEMU results (Forgejo CI runs 27-32, 1100D.105, no PM model yet)

| scenario | WFI/s | IRQ/s | task sw/s | busy | hottest PC | Canon counter at window end | ML holds | [PM] calls in window |
|---|---:|---:|---:|---:|---|---:|---:|---:|
| canon | 0 | 105 | 69 | 1.00 | 0xFF1EA818 | 0 | – | 0 |
| ml-full | 108 | 112 | 341 | 0.02 | 0xFF1EA9B4 (after WFI) | 1 | 0 | 116 |
| ml-hold | 10 | 109 | 329 | 0.91 | 0xFF1EA818 | **0** | **0** | 116 |

- The hold build ends boot with `[PS] release boot -> 0` and Canon's counter at 0; the PowerMgr task then sits in
  the mode-2 loop (0xFF1EA818) exactly like Canon alone, while `ml-full` stays at Counter = 1 and plain WFI. ML's
  tasks keep their rates (task switches/s within 4 % of `ml-full`), so nothing stalls; the busy ratio rises only
  because qemu-eos does not model the PM registers and the mode-2 loop spins (same as `canon`).
- The ML menu opened after the window takes `[PS] hold menu -> 1` as expected.
- **Finding:** with ML loaded (both builds) Canon's `PropMgr` toggles Disable/Enable about twice a second at idle
  (Canon alone: never). The task switch preceding each is `clock_task`, whose `do_this_every_second()` calls
  `_prop_lv_lens_request_update()` once a second outside LiveView (`src/shoot.c:390`). Every `EnablePowerSave`
  restamps PowerMgr's "last" time and the deep sleep needs ≥ 1000 ticks without one, so this 1 Hz lens-info request
  probably keeps the camera out of self-refresh even with ML's holds at 0. Fixed on branch `power/lens-poll`,
  see *Follow-up: lens info poll outside LiveView* below.

### QEMU recipe (1100D.105)

The QEMU side (PM register model) is being done separately in the QEMU-EOS-Optimized repo; steps 3–5 need it.
Until then the PowerMgr mode-2 loop spins at 0xFF1EA818 in emulation (`docs/EMULATION.md`), exactly as for the
`canon` scenario, so steps 1–2 work today and steps 3–5 are the target.

1. **Build.** `cd platform/1100D.105 && make clean && make -j4 POWERSAVE_HOLD=y` (CI does this automatically:
   `ci/qemu/ci-emulation.sh` builds `1100D-hold.zip` next to `1100D-full.zip`, and `run_cam.sh` runs the extra
   scenario `ml-hold`).
2. **Counter at idle.** Boot (`WARMUP_S` 40 s) and read the DryosDebugMsg log (`emu-out/1100D/ml-hold/qemu.log`):
   - expect `[PS] hold boot -> 1` then `[PS] release boot -> 0`, and Canon's last
     `[PM] EnablePowerSave (Counter = 0)`;
   - `comparison.md` prints `ml-hold power save: Canon counter (last [PM] msg) = 0 … ML holds (last [PS] msg) = 0`;
     `ml-full` stays at Counter ≥ 1 for the whole run;
   - RAM check with the GDB stub (`-s -S`, `arm-none-eabi-gdb`, `target remote :1234`): `x/wx 0x2B64` == 0 at idle
     (PowerSaver counter; 0x2B60 = init magic) and `x/wx 0x750C` == 2 (PowerMgr mode).
   - Open the ML menu (`sendkey delete`): `[PS] hold menu -> 1`, `[PM] DisablePowerSave (Counter = 1)`, 0x2B64 == 1;
     close it: back to 0. Debug → *Powersave holds* shows `0` at idle and `1` with `menu` in its help line.
3. **Deep sleep (needs the PM model).** With the display off (`sendkey` INFO until off, or after Canon's display
   timeout) and the image engine clock off (0xC0400008 bit 1 clear), expect `[PM] pmSelfRefresh : In` / `Out` in
   the log, the wakeup plugin's halted time to rise, and `wakeups.json` to show long idle periods. With `ml-full`
   there must be **no** `pmSelfRefresh` at all (counter pinned ≥ 1).
4. **Intervalometer with the display off.** Set Shoot → Intervalometer = 10 s, *Start trigger: Leave menu*, and
   leave the menu; turn the display off. Expect `[PS] hold intervalometer -> 1` within one `shoot_task` loop,
   Counter = 1, **no** `pmSelfRefresh` while it runs, and shots on time: GDB breakpoint on `take_fast_pictures` printing
   `get_ms_clock()`; intervals 10 000 ms ± 20 ms (shutter emulation as in `power-changes.md` §5). Stop it (MENU or
   PLAY): `[PS] release intervalometer -> 0`, Counter = 0, then `pmSelfRefresh` may follow after ≥ 1000 ticks.
5. **No task stalls.** With `-d tasks` compare per-task switch counts (`tasks.json`) of `ml-hold` vs `ml-full`
   during step 4: `shoot_task`, `clock_task`, `cls_task` must have the same rate within noise while a hold is taken.
   During an unheld deep sleep their rate drops to 0 and recovers after an injected key (`sendkey`) — expected.
6. **Lua.** Run a script with `task.create` + `msleep` loop (or an `event.seconds_clock` handler): `[PS] hold lua
   task` / `lua events` while it runs, released when it ends; its timing keeps up with the display off.

Hardware (only when the user says so): battery current/runtime at idle with LCD on and off, stock vs `full` vs
photo; intervalometer and bulb timer accuracy with the display off; APO still fires at the configured time.

## Follow-up: lens info poll outside LiveView (branch `power/lens-poll`)

### Cause

Outside LiveView Canon does not stream `PROP_LV_LENS` (focal length, focus distance, focus position). Upstream ML
(f330c6136, 2018) therefore asked the MPU for it once a second from `clock_task` whenever LiveView was off and a
lens was attached (`do_this_every_second()`, formerly `src/shoot.c:388-393` → `_prop_lv_lens_request_update()`,
`src/lens.c`, `prop_request_change(PROP_LV_LENS, …)`). Each request is an MPU round trip; Canon brackets the
SIO/MREQ traffic with `DisablePowerSave`/`EnablePowerSave` in `PropMgr`, so the counter toggled ~3.5×/s at idle and
every `EnablePowerSave` restamped PowerMgr's "last" time, keeping the self-refresh sleep (≥ 1000 ticks quiet) out
of reach even with ML's holds at 0.

### Who uses the data outside LiveView

| Consumer | Where | Visible when |
|---|---|---|
| Info bars (focal length) | `src/lens.c` `focal_len_update`, drawn by the menu at `src/menu.c:4806-4807` | ML menu open |
| Focus → DOF Settings info (focal length, focus distance, hyperfocal, DOF near/far, diffraction) | `src/focus.c:255-332` (`display_lens_hyperfocal`, via `dof_info_update` `:335`) | ML menu open |
| Flexinfo items (focal length, focus distance, DOF, hyperfocal) | `src/flexinfo.c:1691-1780` | Canon photo info screen, display on (`FEATURE_FLEXINFO`; not on 1100D) |
| Lua `lens.focal_length`, `focus_distance`, `focus_pos`, `hyperfocal`, `dof_near`, `dof_far` | `modules/lua/lua_lens.c` | when a script reads them |
| DOF computation | `src/focus.c:145` (`focus_calc_dof`, from `update_stuff` in the property handler) | derived, follows the above |

LiveView-only users (focus-ring triggers in `_lens_dynamic_data_post_update`, movie log, silent-picture metadata,
`mlv_rec`, rack/stack focus) get the property from Canon's own LiveView stream and are unaffected. Exposure
features (ETTR, auto ISO, intervalometer, bulb) do not read these fields.

### Fix

`lens_info_poll_step()` (`src/lens.c`, called from `clock_task` every 200 ms, `src/shoot.c:431-435`) requests the
update only while `lens_poll_wanted()` says a consumer exists:

- LiveView off and a lens attached (as before), **and** one of
- ML menu shown (`gui_menu_shown()`);
- flexinfo screen visible (`DISPLAY_IS_ON && display_idle() && get_global_draw()`, only with `FEATURE_FLEXINFO`);
- any ML power-save hold (`powersave_holders() > 0`): an ML activity runs (script, task, intervalometer, menu
  action, bulb…) and may read `lens_info`; Canon is held anyway, so the extra toggles cost no sleep;
- a Lua read of one of the dynamic fields in the last 10 s (`lens_info_want_fresh()`, `modules/lua/lua_lens.c`).

The first request goes out as soon as a consumer appears (the next 200 ms tick, e.g. right after the menu opens),
then once a second as before, so whatever is on screen is refreshed exactly as upstream. With nobody looking (Canon
shooting screen without flexinfo, display off, playback) ML sends nothing. Same logic in every build (with or
without `CONFIG_POWERSAVE_HOLD`); no new hold is needed. DIGIC 6/7 keep calling the (empty) request hook; DIGIC 8+
is untouched.

### Measurement (Forgejo CI, 1100D.105, 30 s idle window, ML menu opened for 6 s after it)

`scenario.py` now reports Canon's counter changes per second (every Disable/Enable logs `[PM] … (Counter = n)`,
so the count of those lines is exact), samples RAM 0x2B64 through the monitor every ~0.5 s as a cross-check, and
counts `[PM]` calls while the ML menu is open (`--pm-counter-addr`, `--menu-s`; `run_cam.sh` passes 0x2B64 for the
1100D).

| scenario | [PM] changes/s at idle, before (run 37) | after (run 36) | [PM] calls with menu open, before / after | task sw/s before / after | IRQ/s before / after |
|---|---:|---:|---:|---:|---:|
| canon | 0 | 0 | – | 62 / 65 | 100 / 104 |
| ml-full | 3.73 (112) | **0** | 45 / 45 | 332 / 336 | 110 / 108 |
| ml-hold | 3.33 (100) | **0** | 46 / 46 | 298 / 329 | 101 / 106 |

- At idle outside LiveView Canon's counter no longer changes at all with ML loaded: `ml-hold` sits at 0 for the
  whole window (all 59 RAM samples 0), like Canon alone. The 0.5 s RAM samples never caught the old toggles (each
  lasts a few ms), which is why the log count is the metric.
- With the menu open the request rate is unchanged (45/46 `[PM]` calls in 6 s before and after): the visible
  consumer still gets 1 Hz updates.
- No regression in the other metrics: task switches/s, IRQ/s and the per-task rates are within run-to-run noise
  (runner load differs between runs); the PC profile is unchanged (0xFF1EA9B4 for `ml-full`, 0xFF1EA818 for
  `ml-hold`). The self-refresh sleep itself still needs the PM register model (QEMU recipe step 3).

### Risks

1. **Stale values outside LiveView when nobody is "looking".** A consumer not in the list sees the values from the
   last refresh (menu, picture taken, LiveView). Known gaps: a Lua script that registers a `property.LV_LENS`
   handler without holding power save or reading `lens.*` no longer gets 1 Hz events outside LiveView; the first
   `lens.*` read of a script that holds nothing (e.g. from a key handler) returns the previous value, later reads
   within 10 s are fresh. Running scripts and their tasks hold power save, so they are polled as before.
2. **Flexinfo check is coarse.** On cameras with flexinfo the poll runs whenever the photo info screen is visible,
   whether or not a lens item is configured; correct, just not minimal (no effect on the 1100D or EOSM, which is
   always in LiveView when shooting).
3. **Real hardware may toggle for other reasons** (other MPU traffic). QEMU shows none left at idle; confirm on the
   camera with the DryosDebugMsg log before claiming the deep sleep is reached.

