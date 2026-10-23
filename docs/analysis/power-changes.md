# Power changes: low-risk wakeup reductions (EOSM.202)

This file tracks the changes made from the ranked list in `power-audit.md` (§5).
Each section matches one commit and gives the expected gain, the profiles it affects,
a QEMU verification recipe and the regression risks.
Nothing here has run in an emulator or on hardware yet.
The numbers are estimates for LiveView idle with default settings.

"Photo only" means the change is under `#ifdef CONFIG_PROFILE_PHOTO`.
In that case `ML_PROFILE=full` keeps upstream behaviour: same timeouts, same polling, and a byte-identical `autoexec.bin` where noted.

## 0. Common measurement method (QEMU)

All recipes count **task wakeups**, i.e. how often DryOS switches to a given ML task.

1. Build the profile under test (`make clean && make -j4 FATAL_WARNINGS=y [ML_PROFILE=full]`).
   Install `autoexec.bin` and the `ML/` directory on the QEMU SD image as for any qemu-eos run.
2. Start `qemu-eos` for `EOSM` with the GDB stub (`-s -S`) and `-d debugmsg`.
   Boot to the idle Canon screen.
   The audit notes that QEMU has no real LiveView vsync, so measure in the idle photo screen and, where stated, in the ML menu.
3. Attach `arm-none-eabi-gdb platform/EOSM.202/build/magiclantern` (`target remote :1234`).
   Put a breakpoint on `tskmon_task_dispatch` (it runs on every context switch, `init.c:637`).
   Use a GDB Python command that reads `next_task->name`, increments a per-name counter, and continues.
   Dump the counters after a fixed window of guest time, for example 30 s from `get_ms_clock()`.
   Divide by the window to get wakeups/s per task.
   Without GDB, a debug build can print the same counters through `DryosDebugMsg` every 10 s (audit, verification method A).
4. Compare against the same run on the parent commit.
   Only the task named in the section should change.
5. `ci/check_wakeups.py build/magiclantern --forbid ...` lists the tasks linked into `.tasks`.
   It exits 1 if a forbidden one is present.
   It is a static check that runs without an emulator.

The shutdown test used below: boot, wait 10 s, then trigger power-off.
qemu-eos sends `PROP_TERMINATE_SHUT_REQ` on the power switch or the monitor `system_powerdown`.
Expect `ml_shutdown()` to complete: `config_save()` writes `ML/SETTINGS/MAGIC.CFG`, `module_shutdown()` removes the module lockfile, and the guest powers off.
GDB: break on `module_shutdown` and on `config_save_at_shutdown`. Both must be hit, and QEMU must exit cleanly.

## 1. Video-only boot tasks removed from the photo profile (audit row 2)

**Change.** The profile commit already drops `fps_task` (`FEATURE_FPS_OVERRIDE`), `bitrate_task` (`FEATURE_NITRATE`) and `audio_common_task` from the photo build.
That commit also removes their init functions and menus (`fps_init`, `bitrate_init`, `hdr_init`, `crop_mode_hack_init`, `audio_menus_init`).
This commit also removes `vignetting_init`, a one-shot boot task with an 8 KB stack.
It prepares the movie-only vignetting correction, and that correction cannot be enabled without its menu (`lv-img-engio.c`).
`ci/check_wakeups.py` makes the result checkable.

**Profiles.** Photo only (`FEATURE_*` already undefined there). Full: unchanged, `autoexec.bin` is still 454,976 B.

**Gain.** fps 10/s + bitrate 1/s = **11 wakeups/s** (plus audio 100/s, already gone), and one fewer boot task.
The photo build links 14 boot tasks (full: 18).

**Verify.**
- Static: `ci/check_wakeups.py build/magiclantern --forbid fps_task bitrate_task audio_common_task vignetting_init` passes for photo and fails for full.
- QEMU (method 0): no dispatches named `fps_task`, `bitrate_task`, `audio_common_task` or `vignetting_init` in the photo build.
- Menus: the Movie tab shows no FPS, bitrate or vignetting entries.

**Risks.**
- `fps_task` also called `bv_auto_update()` after FPS changes (`fps-engio.c:1869`).
  Without FPS override, ML never changes the FPS, and `bv_auto_update()` still runs from its property handlers (`lens.c`). No loss.
- A `magic.cfg` written by a full build may have `digic.vignetting.corr=1`.
  In the photo build, `vignetting_correction_initialized` stays 0, so `vignetting_correction_apply_regs()` returns early. It is also movie-mode only.

## 2. `tweak_task`: 20 Hz only when needed (audit row 5). Bug fix, both profiles

**Change** (`tweaks.c`, `gui-common.c`).
Upstream re-armed the "2 s responsive" countdown on every loop while the display was on, so `tweak_task` always ran at 20 Hz.
Now the 50 ms period applies only in these cases:
- for 2 s after a user key event (`tweak_task_wakeup()`, called next to `idle_wakeup_reset_counters()` in `handle_common_events_by_feature`);
- for 2 s after any display on/off transition, which keeps the original intent of the comment;
- while recording, sticky half-shutter or sticky DOF is on;
- during timelapse playback;
- while a display tweak that fights Canon overwrites is on: LV brightness, contrast, saturation, crazy colours, DIGIC peaking, colour scheme, image position, upside-down, grayscale peaking. All are off by default.

Otherwise it sleeps 500 ms.
The sleep is a `take_semaphore(tweak_wake_sem, …)`.
The key-event hook gives the semaphore when the task is in slow mode, so the first key press is served at once instead of after up to 500 ms.
The semaphore is given only on the slow→fast edge, so its count stays bounded.

**The bottom-bar idle reset was not moved** into `handle_other_events`.
Canon draws the bar after it handles the event, so a check inside the event handler would miss the first dial click.
The bar stays on screen for about 2 s, so the existing check in the 2 Hz loop still catches it.
Moving the check would not remove any wakeup.

**Profiles.** Both. This is the bug fix; the full `autoexec.bin` grows by 256 B.

**Gain.** LV idle, display on, defaults: 20 → 2 wakeups/s, so **−18/s**.
With the display off it was already 2/s.

**Verify (QEMU, method 0).**
1. Idle photo screen for 30 s: `tweak_task` ≈ 2/s (parent commit: ≈ 20/s).
2. Inject a key (`sendkey` for an arrow or INFO in the qemu monitor).
   For the next 2 s the rate is ≈ 20/s, then it falls back to 2/s.
   GDB: break on `tweak_task_wakeup` and check that it is hit once per key press.
3. Enable *Display → LV saturation = Grayscale*.
   The rate stays at ≈ 20/s while idle.
   Set it back and the rate returns to 2/s after 2 s.
4. Toggle the display (INFO until the display is off, or the `DISPLAY_IS_ON` property): 2 s at 20/s, then 2/s.

**Risks.**
- State changes not caused by a key and not covered by a display tweak are now handled within ≤ 500 ms instead of ≤ 50 ms.
  Examples: Canon leaving QR review on a timer, the bottom bar after an exposure change, the warnings in `warn_step`.
  These are visual and heuristic only.
- `ISO_ADJUSTMENT_ACTIVE` and the bottom-bar idle reset are polled at 2 Hz.
  That is far below the powersave countdown resolution (seconds).
- On hardware, check quick zoom and focus-box speed in playback.
  Both are key-driven and get a direct wakeup, so no change is expected.
