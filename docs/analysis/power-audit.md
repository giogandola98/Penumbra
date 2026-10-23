# Power audit: Magic Lantern on EOS M (EOSM.202), photo use

Scope: the upstream `dev` tree as imported into this fork, built for `platform/EOSM.202`.
The audit covers only the features a photo user needs: zebras, focus peaking, histogram, magic zoom,
intervalometer, bracketing/HDR, bulb timer, auto-ETTR, dual ISO, trap focus and optional Lua.
The audit made no source changes.

**Evidence labels.** **[V]** means verified in source or in the linked binary, with a `file:line` reference.
**[I]** means inference: plausible, but it needs emulation, RE or hardware confirmation.
Wakeup rates assume that DryOS `msleep(10)` is one 10 ms tick (`MIN_MSLEEP 10`, `platform/EOSM.202/consts.h:172`) **[I]**,
and that the EOS M LiveView display refreshes at about 30 Hz **[I]**.

## 1. What is actually compiled for EOSM.202

- Build: `make -C platform/EOSM.202 FATAL_WARNINGS=y` succeeds. `autoexec.bin` is 454,976 B.
- `features.h` includes `all_features.h` and only removes the features listed in `platform/EOSM.202/features.h`.
  **`FEATURE_TRAP_FOCUS` is undefined** (`features.h:22`), so trap focus is not available today **[V]**.
  `FEATURE_AUDIO_REMOTE_SHOT` is added (`features.h:16`).
- `config-defines.h:22,25,31` enables `CONFIG_CRASH_LOG`, `CONFIG_AUTOBACKUP_ROM` and `CONFIG_TSKMON` for every camera.
- `arm-none-eabi-nm build/magiclantern` lists **18 `TASK_CREATE` tasks** (`.tasks` section, started by `init.c:506-525`),
  **35 `INIT_FUNC`s** (run once at boot) and **151 property handlers** (`.prop_handlers`) **[V]**.
  Property handlers run in Canon's PropMgr task and add no ML wakeups. Their cost is per property change.
- Dynamic tasks:
  - `ml_init` (`init.c:713`) runs once.
  - `ml_backup` (`init.c:492`) runs once, on first boot only.
  - `run_test` (`menu.c:7179`) runs on demand.
  - Module tasks start only when a module is enabled and the feature is triggered:
    `ettr_task` (`ettr.c:852,1473`), `adv_int_task` (`adv_int.c:744`), `lua_*` tasks and `filecopy_task`.
- **Modules are off by default.** A module loads only if its `.en` flag file exists (`module.c:597-611`) **[V]**.
  Once loaded, modules hook existing ML tasks through CBRs:
  - `CBR_SHOOT_TASK`: ettr, dual_iso, lua, silent, crop_rec.
  - `CBR_VSYNC` (per frame): lua, silent, mlv_lite.
  - `CBR_SECONDS_CLOCK`: lua.
- `modules.included` ships these video and toy modules: `mlv_lite`, `mlv_play`, `mlv_snd`, `crop_rec`, `arkanoid`, `bench`, `selftest`, `edmac`.
  They have no runtime cost unless the user enables them.

## 2. Task inventory and wakeups

Each `msleep` return or semaphore/queue timeout counts as one wakeup.
Columns: **LV** = LiveView idle with default config; **Menu** = ML menu open; **Off** = LV running with the display off;
**Play** = playback.

| Task (prio, stack) | Wake mechanism (file:line) | LV | Menu | Off | Play | Work per wake | Runs when feature off? | Photo needs? |
|---|---|---|---|---|---|---|---|---|
| `audio_common_task` (0x18, 4K) | `msleep(MIN_MSLEEP)` (`audio-common.c:545`) | **100** | 100 | 100 | 100 | reads audio MMIO 0xC0920110 for 2 channels (`audio.h:635-643`); meters every 5th wake | **yes**: meters drawn only in movie mode (`audio-common.c:351`) | only for audio remote shot |
| `livev_hiprio_task` (0x18, 16K) | vsync queue, 100 ms timeout (`vsync-lite.c:20`) plus `msleep(10)` (`zebra.c:4167`); `msleep(100)` poll when not drawing (`zebra.c:4088-4091`) | **~60** | 10 | 10 | 10 | zebras, peaking, MZ, spotmeter, lens bar | throttles to 10 Hz polling | yes (overlays) |
| `shoot_task` (0x1a, 8K) | `msg_queue_receive(..., 50/200)` (`shoot.c:5474-5488`) | **20** | 20 | 5 | 20 | ~25 feature checks, `misc_shooting_info` every 5th, `CBR_SHOOT_TASK` | yes | yes |
| `tweak_task` (0x1e, 4K) | `msleep(50 or 500)` (`tweaks.c:969-974`) | **20** | 20 | 2 | 20 | movtweak, warnings, display presets, bottom-bar heuristic | yes | barely |
| `cls_task` (0x1a, 8K) | `msleep(100)` (`zebra.c:3809`) | 10 | 10 | 10 | 10 | LED blink, clear-overlay, `idle_powersave_step`, `cropmark_step` | yes | partly |
| `focus_misc_task` (0x1e, 4K) | `msleep(100)` (`focus.c:1024`) | 10 | 10 | 10 | 10 | decrements `hsp_countdown`; trap-focus code is compiled out | yes | no (on EOSM) |
| `fps_task` (0x1c, 4K) | `msleep(100)` twice (`fps-engio.c:1841,1862`) | 10 | 10 | 10 | 10 | reads FPS timers | yes | **no** (video) |
| `clock_task` (0x19, 8K) | `msleep(200)` (`shoot.c:439`) | 5 | 5 | 5 | 5 | µs clock, `CBR_SECONDS_CLOCK`, per-second jobs | n/a | yes, replaceable |
| `livev_loprio_task` (0x1f, 32K) | `msleep(200)` twice (`zebra.c:4246-4249,4298,4309`) | 5 | 5 | 5 | 5 | histogram, waveform, vectorscope | yes | yes (histogram) |
| `debug_task` (0x1e, 8K) | `msleep(200)` (`debug.c:672`) | 5 | 5 | 5 | 5 | screenshot countdown, crash-log check | yes | no |
| `console_task` (0x1d, 4K) | `msleep(200)` (`console.c:314`) | 5 | 5 | 5 | 5 | draws console if visible | yes | no |
| `module_task` (0x1e, 16K) | queue, 200 ms timeout (`module.c:2204`) | 5 | 5 | 5 | 5 | nothing on timeout | yes | no on timeout |
| `menu_task` (0x1a, 8K) | `take_semaphore(gui_sem, 500)` (`menu.c:5777`) | 2 | 2 (+ full redraw) | 2 | 2 | config autosave check; redraw every 500 ms when open | yes | yes |
| `menu_redraw_task` (0x1a, 32K) | queue, 500 ms timeout (`menu.c:4942`) | 2 | 2 | 2 | 2 | nothing on timeout | yes | yes |
| `notifybox_task` (0x1b, 4K) | `take_semaphore(.., 500)` (`notify_box.c:29`) | 2 | 2 | 2 | 2 | nothing on timeout | yes | yes |
| `focus_task` (0x18, 4K) | `take_semaphore(.., 500)` (`focus.c:748`) | 2 | 2 | 2 | 2 | nothing on timeout | yes | focus stacking only |
| `bitrate_task` (0x1d, 4K) | `msleep(1000)` (`bitrate.c:648`) | 1 | 1 | 1 | 1 | `bitrate_set()` returns early in photo mode | yes | **no** (video) |
| `vignetting_init` | runs once (`lv-img-engio.c:1146-1153`) | 0 | 0 | 0 | 0 | sets coefficients | - | - |
| Canon `gui_main_task` (ML override) | `msg_queue_receive(.., 0)`, waits forever (`gui.c:128`) | event | event | event | event | button/event filtering | - | yes |

**Estimated total at idle in LiveView with defaults: about 264 ML wakeups/s** (sum of the LV column) **[I]**.
That is about 181/s with the display off and about 230/s in playback.

- About 111/s comes from video-only tasks: audio 100, fps 10, bitrate 1.
- About 60/s comes from the overlay task's double wake per frame.
- Six tasks wake only to time out and do nothing: about 18/s in total.

`tskmon_task_dispatch` also runs on **every** context switch (`init.c:637`, `tskmon.c:368-428`).
It updates timers, reads address 0 and can scan stack memory.
Once per second, `do_this_every_second()` re-arms a stack-magic scan for all tasks (`shoot.c:361-364`, `tskmon.c:222-226`) **[V]**.

## 3. Sleep and power-saving blockers

### 3.1 `call("DisablePowerSave")` at boot, never re-enabled. Highest RE priority

`my_big_init_task` calls `call("DisablePowerSave")` unconditionally (`init.c:478`).
No call to `EnablePowerSave` exists anywhere in `src/` or `modules/` **[V]** (`git log -S` shows it was removed long ago).

The semantics of this Canon eventproc on EOSM 2.0.2 are **unknown [I]**.
It may only inhibit Canon's auto power off (APO) until Canon re-arms it, or it may disable deeper CPU or LCD power saving for the whole session.
The PLAN's note that "ML disables CPU powersave" may point here.

**Action:** disassemble `DisablePowerSave` and `EnablePowerSave` in ROM1 with Ghidra (phase 2).
Then trace in QEMU whether APO and the idle path still fire after boot.

### 3.2 Calls that reset or block Canon's APO timer

All of these go through `PROP_ICU_AUTO_POWEROFF` (`powersave.c:12-31`) **[V]**.

| Call site | Effect | Assessment |
|---|---|---|
| `lens.c:1852-1858,1882` `focus_ring_powersave_fix` | `powersave_prolong()` on each `PROP_LV_LENS` focus-position change in MF | Correct intent. If an EF-M focus-by-wire lens jitters `focus_pos`, APO never fires **[I]**. Debounce it. |
| `powersave.c:504-514` (30-min timer) | prolongs every 10 s and blinks the LED | Opt-in (`idle.dis.30min` default 0). Works only when APO is disabled. |
| `menu.c:7163-7168`, `file_man.c:518,574`, `lua.c:780,874`, `lua_task.c:51,67` | prohibit, then permit | Balanced. A Lua script running in background keeps APO prohibited only while it loads (`lua.c:874`). |
| `fake_simple_button(BGMT_*)` (89 sites) | Canon key events go to Canon handlers (`gui.c:150-165`) and probably reset APO **[I]** | No periodic Canon-key fakes at default settings. `tweaks.c:1169` fakes INFO only with display presets. Menu key-repeat fakes only while a key is held. |
| `redraw()` → `GUI_Control(MLEV_REDRAW)` (`zebra.c:3984-3987`, `gui-common.c:693-704`) | ML events are swallowed before Canon (`gui.c:155-157`) | Whether `GUI_Control` itself resets APO is unknown **[I]**. The overlay task calls `redraw()` on every state transition. |

### 3.3 What `powersave.c` does, and its gaps on EOSM

- **Everything is opt-in.** Dim, LCD off, GlobalDraw off, clear-overlays and the 30-min override all default to 0 (`powersave.c:131-136`).
  With defaults, ML adds no display or LV power saving and relies on Canon APO **[V]**.
- **It is a polling state machine.** It runs at 10 Hz from `cls_task` (`zebra.c:3866-3868`) using countdowns in 100 ms units (`powersave.c:216-277`).
  Any non-idle state (menu, Canon dialog, half-shutter) resets the countdowns (`powersave.c:256-264`).
  A one-shot timer re-armed from `handle_buttons` (`gui-common.c:481-485`) would do the same with no polling.
- **"Turn off LCD" pauses LiveView** through `PROP_LV_ACTION` (`powersave.c:38-57,318-327`).
  This pause has never been validated on EOSM: `FEATURE_FORCE_LIVEVIEW` is undefined there (`features.h:3`) **[I]**.
  Resume uses `wait_lv_frames` (`powersave.c:77-78`).
- **Dimming does not check auto brightness on EOSM.** The guard is compiled out because `CONFIG_AUTO_BRIGHTNESS` is not defined (`internals.h:47`, `powersave.c:353`).
- **The intervalometer depends on this module.** `idle_force_powersave_now()` (`shoot.c:6064,6125`) only shortens the countdowns.
  The countdowns act only if "Turn off LCD" or "Dim" is configured (`powersave.c:442-449`).
  With defaults, LV and the LCD stay on between timelapse shots.
  If the user disables APO, as the menu warns them to (`shoot.c:476-487`), the sensor and LCD stay on for hours.
- **The idle LED blinks every 5 s** while the display is off, on by default (`powersave.c:110,122-124`).
  It is a small cost, and it keeps `cls_task` relevant when the display is off.
- **No coordination with Canon APO.** ML countdowns and Canon APO run independently, and nothing hands APO a "you may sleep now" signal.

### 3.4 Busy loops

- Intervalometer last second: `while (SECONDS_REMAINING > 0) msleep(10);` (`shoot.c:6079-6082`).
  That is up to 100 wakeups per shot. Before it, a 200 ms loop prints a `bmp_printf` status every iteration, even with the display off (`shoot.c:6016-6052`).
- `audio_common_task` at 100 Hz (above) is effectively a busy poll.
- `hold_your_horses` and `state_init` have startup-only polls (`init.c:538-544`, `state-object.c:292`). Negligible.
- No code disables CPU idle (`wfi`) on EOSM. The `wfi` sites in `cpu.c:154,196` are DIGIC 7/8 SMP code **[V]**.

## 4. LiveView overlay hot paths

**Redraw strategy: everything is time-driven, never change-driven [V].**

- `livev_hiprio_task` wakes on each display vsync (`zebra.c:4146`, posted from `state-object.c:158`).
  It then sleeps 10 ms (`zebra.c:4167`) and calls `draw_zebra_and_focus(k%3==0 or k%5==0, k%2==1)` (`zebra.c:4184-4191`).
  It never checks whether the image, exposure or overlay settings changed.
  - **Zebras (default on, LumaFast).** The DIGIC zebra register does the per-frame work in hardware (`zebra.c:1196-1210`).
    The CPU only rewrites the stripe mask once per second: a full 720×480 bitmap pass of about 86k word ops (`zebra.c:1214-1248`). This is cheap.
  - **Focus peaking (default off).** A Laplacian runs on every 2nd pixel of every 3rd row, about 57k kernel evaluations, every other frame (`zebra.c:1800-1816`).
    That is roughly 0.9 M kernel evaluations/s, plus a dirty-pixel restore pass (`zebra.c:1747-1764`).
    This is the most CPU-heavy overlay **[V code; I magnitude]**.
  - **Spotmeter** (default on): 1 Hz (`zebra.c:4196-4198`).
    **Lens/bottom bar**: twice every 100 iterations, or every 10 when dirty (`zebra.c:4219-4242`).
- `livev_loprio_task` runs `hist_build()` every 400 ms, unconditionally.
  `hist_build()` scans every 2nd pixel of every 2nd row (about 86k pixels) and does an RGB histogram per pixel (`zebra.c:530-614`, `4298-4313`).
  Waveform and vectorscope share that scan.
- **Default RAW histogram switches LV to RAW mode.** The default `hist.type` is 2 (RAW) because `FEATURE_RAW_HISTOGRAM` is on (`histogram.c:28`, `all_features.h:95-97`).
  Shooting RAW (or RAW+JPEG) in photo mode at 1× sets `raw_needed=1`, so the overlay task calls `raw_lv_request()` and keeps it held (`zebra.c:4110-4141`).
  From then on, every EVF frame runs `vsync_func()` → `raw_lv_vsync()`, which programs the raw path and **EDMAC-copies the full raw LV frame** (`state-object.c:221`, `raw.c:2145-2175`).
  So a RAW shooter pays continuous memory bandwidth for a histogram that refreshes at 2.5 Hz **[V mechanism; I magnitude]**.
- Other per-frame hooks run inside Canon's state-machine context in `vsync_func`: `digic_iso_step`, `hdr_step`, `vignetting_correction_apply_regs` and module `CBR_VSYNC` (`state-object.c:85-120,240-248`).
  Each is cheap when idle.

## 5. Ranked optimisation opportunities

**Score** = impact (1-5) × confidence (1-3) ÷ risk (1-3).

Wakeup savings below are for LV idle with defaults.

- **Verification method A** (applies to all items): build a debug variant that counts dispatches per task.
  The dispatch hook already exists (`init.c:637`, `tskmon.c`). Dump the counts every 10 s through `DryosDebugMsg` and run `qemu-eos -d debugmsg`.
  Alternatively, attach GDB to QEMU with a counting breakpoint on `my_task_dispatch_hook`.
- **Caveat:** QEMU produces no LV vsync, so `livev_hiprio_task` falls back to its 100 ms timeout there. Its per-frame cost must be modelled on the host.

| # | Opportunity | Files / functions | Proposed change | Est. gain | Score | Verify | Regression risk |
|---|---|---|---|---|---|---|---|
| 1 | **Stop the 100 Hz audio poll** | `audio-common.c:528-558`; `shoot.c:6140-6198` | Photo build: drop `FEATURE_AUDIO_METERS`. Keep audio remote shot by computing levels only while `audio_release_running` (start/stop the task or block on a semaphore). | −100 wakeups/s (~38 %) [V] | 5×3÷1 = **15** | A: task count 100/s → 0 | Low. Audio remote shot must still trigger; test it on hardware last. |
| 2 | **Remove video-only tasks from the photo build** | `fps-engio.c:1822-1995`, `bitrate.c:625-653` (`FEATURE_FPS_OVERRIDE`, `FEATURE_NITRATE`); also `FEATURE_REC_NOTIFY` (`zebra.c:4207-4211`) | `#undef` in the photo profile. Check that `vsync_func`'s FPS hooks compile out (`state-object.c:107-111`). | −11/s, 8 KB stack [V] | 3×3÷1 = **9** | A; `nm` must not show `fps_task`/`bitrate_task` | Low. `bv_auto_update` (expo override) is called from `fps_task` (`fps-engio.c:1869-1871`); move it to a prop handler. |
| 3 | **Do not keep LV in RAW mode for the default histogram** | `zebra.c:4110-4141`, `histogram.c:27-31`, `raw.c:2145` | Default `hist.type` = YUV RGB. Or take a one-shot raw snapshot on demand (request → 1 frame → release, as `ettr.c:1276-1424` does) at ≤1 Hz, or only on half-shutter. | Removes a per-frame raw EDMAC copy for RAW shooters [V mech, I size] | 4×2÷1 = **8** | Host: histogram equality YUV vs RAW on recorded frames. QEMU: `raw_lv_request_count`==0 at idle. | Low-med: RAW histogram users lose live accuracy; keep it as an option. |
| 4 | **Overlay task: one wake per frame, frame-rate cap, change gating** | `zebra.c:4044-4243` (`_lv_vsync`, `msleep(10)`, `draw_zebra_and_focus`) | Remove the extra `msleep(10)` (or use it only when MZ is on). Cap overlay rate to ~10 Hz when only zebras/spotmeter are on. Skip peaking when lens focus/zoom/exposure props have not changed for N frames. | −40…50/s; peaking CPU −50…70 % [I] | 4×2÷2 = **4** | A (host model for vsync); host benchmark of peaking at decimation levels | Medium: MZ smoothness, zebra/peaking latency, flicker. |
| 5 | **Make `tweak_task` 2 Hz** | `tweaks.c:966-974` | Bug: `display_countdown` is re-armed to 40 on **every** loop while the display is on, so the "2 s responsive" path is permanent. Use 50 ms only while `halfshutter_sticky`/recording. Move the bottom-bar idle reset (`tweaks.c:1174-1186`) into `handle_other_events`. | −18/s [V] | 3×3÷1 = **9** | A | Low: palette or warning changes are a few hundred ms slower. |
| 6 | **Infinite waits for event-driven tasks** | `module.c:2204`, `menu.c:4942`, `notify_box.c:29`, `focus.c:748`, `menu.c:5777` (when closed and no key-repeat) | Use timeout 0 (forever). Wake them from `ml_shutdown_requested` by posting a message or giving the semaphore. Move menu config autosave into `menu_close()`. | −13/s [V] | 2×3÷1 = **6** | A, plus a QEMU shutdown test: power off, then `module_shutdown()` completes. | Low-med: the shutdown path needs explicit wakeups. |
| 7 | **Drop or merge housekeeping polls** | `focus_misc_task` (`focus.c:1019-1053`), `debug_task` (`debug.c:633-674`), `console_task` (`console.c:288-318`) | Turn `hsp_countdown` into a timestamp. Run crash-log/format-hijack checks from the GUI event or prop handlers. Wake the console only when `console_visible`. | −20/s [V] | 3×3÷2 = **4.5** | A; crash-log selftest in QEMU | Low-med: crash-log capture timing. |
| 8 | **`DisablePowerSave` at boot** | `init.c:478` | RE first. If it inhibits APO or CPU power saving, remove it or pair it with `EnablePowerSave` after init. | Unknown, potentially the largest [I] | 5×1÷2 = **2.5** (rises to 7.5 once RE confirms) | Ghidra on ROM1; QEMU: APO fires after the configured time with ML loaded. | Unknown until RE: it may exist to protect boot. |
| 9 | **Timelapse power** | `shoot.c:6010-6126`, `powersave.c:184-189,318-334` | Arm a one-shot timer instead of the 10 ms spin. Skip `bmp_printf` when `!DISPLAY_IS_ON`. In the photo profile, pause LV and turn off the display between shots by default when the interval is ≥ 5 s. | Display + sensor off for most of a timelapse [I] | 5×2÷2 = **5** | QEMU wakeups during an intervalometer run; hardware A/B last | Medium: LV pause/resume on EOSM is unverified; shot-timing jitter. |
| 10 | **Timers instead of 10 Hz/5 Hz pollers** | `cls_task` (`zebra.c:3799-3877`), `clock_task` (`shoot.c:419-442`) | Make the powersave countdowns one-shot `SetTimerAfter` timers (`gui-common.c:711-714`) re-armed on key events. Compute the seconds clock lazily from `GET_DIGIC_TIMER`, with a 1 s timer for `CBR_SECONDS_CLOCK`. Cropmarks redraw on dirty only. | −13/s [V] | 3×2÷2 = **3** | A; powersave countdown tests in QEMU (key-injection) | Medium: timer callbacks run in interrupt/timer context and must not draw. |
| 11 | **Event-driven `shoot_task`** | `shoot.c:5439-6200` | Post to the existing `shoot_task_mqueue` on prop/key events. Poll at 50 ms only when a polling feature is active: bulb timer armed, HDR delay, motion detect, intervalometer, audio release, or a module CBR that declares it needs polling. | −15/s [V] | 3×2÷3 = **2** | A; feature tests for bulb/HDR/intervalometer via key-injection | High: ETTR, dual ISO and Lua CBRs assume a 20 Hz tick. |
| 12 | **Histogram and peaking algorithm cost** | `zebra.c:530-614`, `1765-1860` | Phase 6: word-wide ops, LUTs, histogram at 1 Hz or on exposure change. | CPU only [I] | 2×2÷1 = **4** | Host unit tests, bit-exact against reference | Low. |
| 13 | **Release build without tskmon** | `config-defines.h:31`, `all_features.h:298` | Keep tskmon in the debug/measurement build only. | per-switch overhead [V] | 2×3÷2 = **3** | A in a debug build (tskmon is the measurement tool) | Low; it loses the on-camera CPU-usage menu. |
| 14 | **Debounce focus-ring APO prolong** | `lens.c:1852-1883` | Prolong only if `|Δfocus_pos|` exceeds a threshold, and at most once per 10 s. | Lets APO fire with a jittery lens [I] | 2×1÷1 = **2** | GDB breakpoint on `prop_request_change(0x80030024)` in QEMU idle | Low. |
| 15 | **Idle LED blink default off** | `powersave.c:110` | Default `idle.blink` = 0 in the photo profile. | LED energy [V] | 1×3÷1 = **3** | Config default test | None. |

**Expected result.** Items 1, 2, 5, 6 and 7 are all low-risk and together cut about 160 of the ~264 idle wakeups/s.
Adding item 4 and item 10 brings the target below 50/s, before any feature trimming or RE findings.
Items 3 and 9 are the largest *energy* wins for RAW shooters and timelapses, because they turn off hardware (EDMAC, sensor, LCD) rather than just CPU wakeups.

## 6. Open questions for the next phases

1. What does `DisablePowerSave` do on EOSM 2.0.2? Is the APO timer still armed afterwards? (Ghidra, phase 2.)
2. The real LV vsync rate on EOS M in photo mode, and how often `PROP_LV_LENS` fires (rate and jitter) with EF-M lenses. (Hardware log, phase 8.)
3. Does `PROP_LV_ACTION` pause or resume LV reliably on EOSM? (QEMU, then hardware.)
4. Does `GUI_Control` with ML (negative) event codes reset Canon's APO counter? (RE.)
5. Trap focus is compiled out on EOSM (`features.h:22`). Re-enabling it is a separate feature task, and in LV it relies on `get_lv_focus_confirmation()` polled at 20 Hz from `shoot_task` (`shoot.c:5833-5863`).
