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
