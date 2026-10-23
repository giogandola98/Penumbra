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

## 3. Event-driven tasks wait forever (audit row 6). Photo only

**Change.**
`tasks.h` adds `IDLE_WAIT_MS(ms)`: 0 (wait forever) in the photo profile, `ms` otherwise.
It also adds `WOKEN_FOR_SHUTDOWN()`: `ml_shutdown_requested` in the photo profile, constant 0 otherwise.
These waits use the new macro:

| Task | Wait | Old timeout | Woken by |
|---|---|---|---|
| `module_task` | `msg_queue_receive(module_mq)` (`module.c`) | 200 ms | offline-string load/unload messages |
| `menu_redraw_task` | `msg_queue_receive(menu_redraw_queue)` (`menu.c`) | 500 ms | `menu_redraw()` and the `menu_redraw_full()` posts |
| `notifybox_task` | `take_semaphore(notify_box_show_sem)` (`notify_box.c`) | 500 ms | `NotifyBox()` |
| `focus_task` | `take_semaphore(focus_task_sem)` (`focus.c`) | 500 ms | follow-focus and rack-focus requests |
| `menu_task` | `take_semaphore(gui_sem)` (`menu.c`) | 500 ms (when closed) | menu toggle requests |

`menu_task` waits forever only when **all** of these hold: the menu is closed, no key repeat is active, arrow shortcuts are inactive, no menu-flag reload is pending, and no autosave is pending (`config_autosave && (config_dirty || menu_flags_save_dirty)`).
Pending work keeps the old 500 ms poll.
So the autosave after closing the menu, and the autosave deferred while recording, run at the same time as before.
**The autosave was therefore not moved into `menu_close()`.**
`menu_close()` runs before the menu is fully gone, and it would also need the existing "not while recording" deferral, which the poll already handles.
A menu-flag reload requested by `menu_add()` while the task sleeps (for example, a module adding menus late) now runs just before `menu_open()`.

**Shutdown.** `ml_shutdown()` (`tasks.c`) sets `ml_shutdown_requested` and then calls `tasks_wakeup_for_shutdown()`.
That function posts a dummy message to `module_mq` and `menu_redraw_queue`, and gives `gui_sem`, `notify_box_show_sem` and `focus_task_sem`.
Each task checks `WOKEN_FOR_SHUTDOWN()` right after it wakes, before it acts on the message.
It then leaves its `TASK_LOOP`, just as it used to after a timeout.
This matters most for `menu_task`, where a given `gui_sem` would otherwise open the menu during shutdown.
`config_save_at_shutdown()` and `module_shutdown()` still run synchronously in `ml_shutdown()`, so they do not depend on these tasks.

**Gain.** module 5 + menu_redraw 2 + notifybox 2 + focus 2 + menu (closed) 2 = **−13 wakeups/s**.
They are now 0/s at idle.

**Profiles.** Photo only.
Full compiles the same timeouts and a constant-false check; its `autoexec.bin` size is unchanged (455,232 B).

**Verify (QEMU).**
1. Wakeups (method 0), idle photo screen, 30 s: `module_task`, `menu_redraw_task`, `notifybox_task`, `focus_task` and `menu_task` show 0 dispatches after boot completes.
   Parent commit: 5, 2, 2, 2 and 2 per second.
2. Menu: open the ML menu (`sendkey` the menu/trash key used by EOSM).
   It opens at once and redraws.
   Change a setting and close the menu.
   Within about 0.5 s, `config_save` is hit (GDB breakpoint) and `ML/SETTINGS/MAGIC.CFG` changes on the SD image.
   After that `menu_task` returns to 0 dispatches/s.
3. Menu flags: star an item in the menu, close the menu, and expect `MENUS.CFG` to be written.
   Load a module, open the menu, and check that its hidden/starred flags are applied on first display.
4. NotifyBox: trigger any notify, for example the screenshot menu item.
   The box appears and is erased after its timeout.
5. Shutdown test (§0): break on `menu_wakeup_for_shutdown`, `notifybox_wakeup_for_shutdown`, `focus_task_wakeup_for_shutdown` and `module_task_wakeup_for_shutdown`.
   All four are hit.
   With `-d tasks` (or `task_dispatch` counting), the five tasks run once more and exit; the menu does **not** open.
   `module_shutdown()` completes (lockfile removed), and QEMU powers off.

**Risks.**
- A config change made while the menu is closed is saved at the next menu close or at shutdown (`config_save_at_shutdown`), instead of within 0.5 s.
  Examples are a Lua script calling `menu_set` or the quick-access menu items.
  A battery pull (`PROP_ABORT`, which never saves) in that window loses the change.
- If arrow shortcuts are active, key repeat keeps the old 500 ms poll (no regression, and no gain in that mode).
- A future task that adds a `give_semaphore(gui_sem)` for anything other than a toggle would be treated as a toggle.
  This was already true before.

## 4. Housekeeping polls (audit row 7). Photo only, except the `hsp` timestamp

**`focus_misc_task`** (`focus.c`).
- On EOSM, trap focus is compiled out (`features.h`).
  The task then did only two things: tick `hsp_countdown` and plot the focus graph after `PROP_LV_FOCUS_DATA`.
- `hsp_countdown` (3 × 100 ms) is now a timestamp, `hsp_time` from `get_ms_clock()`, checked as `< 300 ms`.
  The behaviour is the same and no task has to tick it.
  This applies to both profiles. It is read only under `CONFIG_LV_FOCUS_INFO`, which EOSM does not define, so it is dead code on this camera either way.
- With `CONFIG_PROFILE_PHOTO && !FEATURE_TRAP_FOCUS` (`FOCUS_MISC_EVENT_DRIVEN`), the task blocks on `focus_misc_sem`.
  `update_focus_mag()` gives it on the clean→dirty edge of `focus_graph_dirty`.
  The task clears the flag, plots if the graph is enabled, and sleeps 100 ms, which keeps the old 10 Hz cap.
  The flag is cleared once after the semaphore is created, so an update that arrived before the semaphore existed cannot block later wakeups.
  With trap focus enabled, the old 10 Hz poll stays, because it must watch the AF-mode dialog.

**`debug_task`** (`debug.c`).
- It polls at 200 ms only while there is something to time or watch: the screenshot countdown, the hexdump view, or the Canon menu (`MENU_MODE`, for the format-dialog hijack).
- Otherwise it polls at 1 s.
- Crash/assert logs and core dumps are requested from the assert handler and the task-dispatch hook.
  Those contexts must not give semaphores, so they are not made event-driven.
  `crash_log_step()` and the `ErrForCamera` dialog check keep running, at 1 Hz.

**`console_task`** (`console.c`).
- While the console is hidden and already erased, the task blocks on `console_wake_sem`.
- `console_show()` gives the semaphore.
  `console_visible` is re-read after every wake, and the semaphore counts gives, so a show that races with the check is not lost.
- `console_hide()` needs no wake: the task is already running at 5 Hz while the console is visible, erases it once (`dirty`), and then blocks.
- `ml_shutdown()` also wakes `console_task` and `focus_misc_task` (`tasks_wakeup_for_shutdown`).

**Gain.** focus_misc 10 + debug 5→1 + console 5 = **−19 wakeups/s** at idle.
Inside the Canon menu, `debug_task` stays at 5/s.

**Profiles.** Photo only (`#ifdef CONFIG_PROFILE_PHOTO`), except the `hsp_time` timestamp.
The full `autoexec.bin` size is unchanged (455,232 B).

**Verify (QEMU).**
1. Wakeups (method 0), idle photo screen, 30 s:
   - `focus_misc_task` 0/s (parent: 10/s);
   - `console_task` 0/s (parent: 5/s);
   - `debug_task` ≈ 1/s (parent: 5/s).
2. Open the Canon menu (`sendkey` MENU): `debug_task` ≈ 5/s.
   Go to *Format card*: the "Keep ML" option is drawn as before (`HijackFormatDialogBox_main`).
3. Crash log: from the ML Debug menu, run a test that asserts, or call `ml_assert_handler` through GDB.
   Within about 1 s, `CRASH00.LOG` / `ASSERT00.LOG` appears on the SD image.
4. Console: show it (Debug → Show console, or a Lua `console.show()`).
   It appears and `console_task` runs at 5/s.
   Hide it: the console is erased once, then `console_task` returns to 0/s.
5. Focus graph: QEMU does not emit `PROP_LV_FOCUS_DATA`.
   Inject it with GDB (`call prop_request_change(PROP_LV_FOCUS_DATA, …)`) or break on `update_focus_mag`.
   Each burst wakes `focus_misc_task` at most 10 times per second, and with the graph enabled `plot_focus_mag` is hit.
6. Shutdown test (§0) still completes.

**Risks.**
- A crash or assert log is written up to 1 s later than before (≤ 1.2 s instead of ≤ 0.2 s).
  If the camera locks up within that second, the log can be lost.
  This is the main item to check on hardware, as the audit notes.
- If trap focus is ever re-enabled on EOSM, `FOCUS_MISC_EVENT_DRIVEN` turns off by itself and the 10 Hz poll returns.
