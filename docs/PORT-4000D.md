# Port: Canon EOS 4000D / Rebel T100 (fw 1.1.0)

Status: **in progress** — ML boots in emulation with a photo profile (intervalometer, HDR bracketing, bulb timer,
file manager, image names, Lua; counted powersave holds). Not run on the camera yet. Maintainer's own dump: ROM1 valid (MD5 matches the on-camera dumper), ROM0 is an
unpopulated region (repeated 16-byte bus pattern). No serial flash. Model ID 0x422, DryOS 2.3 r58+p7, ROMBASEADDR
0xFF0C0000. ROMs: private package registry, package `canon-rom`, version `4000D-110` (never in this repository).

Base platform: `1300D.110` (DIGIC 4+, armv5te, `boot-d45-ch.o`, same DryOS generation). Any upstream/community 4000D
work (forum threads, qemu-eos `2000d_test` branch) is checked first and reused only if the license allows (GPL-2.0).

## Progress (branches `port/4000d`, `port/4000d-2`)

| # | Status | Notes |
|---|---|---|
| 1 | done (QEMU-EOS-Optimized) | Canon GUI and menu in emulation; see that repo's `docs/models/4000D.md` |
| 2 | done | 161 of 167 stubs located (152 high, 9 medium confidence), 6 audio/PTP ones missing and not needed by the minimal build; table and method in `firmware/4000D-stubs.md`; `tools/check_stubs.py` re-checks every stub against the ROM in the emulation job |
| 3 | merged into 4 | the normal build is used directly (QEMU scenario `ml-full`) instead of a separate hello-world target |
| 4 | boots in QEMU | `platform/4000D.110` builds with `FATAL_WARNINGS=y` (CI build job). CI run 56 (emulation job): stub check 161/161 OK; `ml-full` and `ml-hold` boot ML (ml_init, menu_task, shoot_task ... in the task log; `[PS] hold boot`, `hold menu` / `release menu` when Delete opens and MENU closes the ML menu); no QEMU errors. Screenshots reviewed (below) |
| 5 | in progress | photo profile (default build, `docs/BUILD-PROFILES.md`): intervalometer, HDR bracketing, bulb timer, modules file_man / img_name / Lua (sd_clock shipped, off), `POWERSAVE_HOLD` on. Each step green in CI emulation (runs 63-67); see *Photo profile* below |
| 6-7 | not started | |

Platform facts used by step 4 (evidence in `firmware/4000D-stubs.md`): main firmware `0xFE0C0000`, kernel copied
to RAM `0x1900..0x4D0AC` by cstart, BSS to `0x829D4`, Canon heap `0x2D0000..0xD00000` (ML takes
`0xC80000..0xD00000` through the cache hack at `0xFE0C1B74`, as on the 1300D), init_task literal at `0xFE0C3A9C`,
firmware signature `0x1B6EF034`, firmware ID `0x80000422`, task structs as `CONFIG_TASK_STRUCT_V2` /
`CONFIG_TASK_ATTR_STRUCT_V4` (read off `_get_task_info_by_id`), new-style task dispatch hook. Display: the BMP
overlay is the usual DIGIC 4 720x480 (`CONFIG_4_3_SCREEN`), scaled by the hardware to the 2.7" 230k-dot panel.

Bring-up feature set (`features.h`, `ML_PROFILE=full`): no LiveView, movie, raw, FPS or audio; debug menus (tasks,
GUI events, free memory, screenshot) only. The photo profile (default) adds the features of *Photo profile*.
LiveView / movie / raw constants: `firmware/4000D-consts.md` (verified / likely / not verified per constant); the
ones still marked "1300D value, not verified" in `consts.h` must be checked before the feature is enabled.

Buttons: the 4000D shares the 1200D/1300D layout (Av and Erase on one key), so `gui.h` starts from the 1300D
codes; the `canon` emulation scenario sends each emulated key once and logs the GUI events Canon derives from it
(`keys.json`), which checks them. Run 56 confirmed every code in `gui.h` that the emulator can send: MENU 6,
INFO 7, PLAY 0xB, Erase 0xD, Q 0x1C, SET 4, up/down/left/right 0x27/0x29/0x25/0x23, zoom in/out 0xE/0x10, and Av
as event 0x61 (`GMT_OLC_INFO_CHANGED`, used by `CONFIG_MENU_WITH_AV`). The emulator's key mapping is borrowed
from the 1200D, so the real camera's MPU codes still need one check (capture below).

Idle in emulation (relative only): canon 213 WFI/s, 86 task switches/s; ml-full 106 WFI/s (Canon counter held at
1), 254-260 switches/s (`audio_common_task` ~50/s although audio is off); ml-photo (run 64) 212 WFI/s as Canon
alone, 141 switches/s (the photo profile does not start the audio task).

### Screenshots (emulation, CI runs 61-67)
- Canon GUI (`canon/boot.png`): the shooting screen at 720x480 (M, 0"3, F25, ISO AUTO, AWB, one-shot, evaluative
  metering, L quality, 35 shots, battery full), Italian UI from the ROM settings. MENU opens the last tab
  (Wi-Fi ... "Versione Firmware 1.1.0"), Q the quick-control screen, PLAY "Nessuna immagine", INFO turns the display
  off (black frame). No offset, scaling or palette problem; the only artefact is a small grey square in the
  bottom-right corner, present in every frame (also Canon-only), which comes from the emulator.
- ML (`ml-*/menu*.png`): Delete opens the ML menu on its beta warning ("development snapshot"); MENU dismisses it
  and the menu shows (`menu_main.png`, Help tab, version / commit / build lines at the bottom). The tab walk shows
  Shoot (Advanced Bracket, Intervalometer, Bulb Timer greyed outside bulb mode, Shoot Preferences), Focus (DOF
  settings, lens "EF-S18-55mm f/3.5-5.6 IS" read from the emulated lens), Prefs, Modules (file_man OK, img_name
  OK, lua OK, sd_clock OFF), Scripts, Debug, Help. Fonts, colours and layout are those of a 720x480 DIGIC 4 camera; nothing is cut
  off or shifted. `menu_set.png` is ML's "Help files not found" page: the test card carries no help files.

### Photo profile
| Feature | Emulation evidence |
|---|---|
| Intervalometer | Shoot tab entry; `ml-photo-ivl` starts it from a config preset: `[PS] hold intervalometer`, Canon counter 1 in all samples, ML holds 1 |
| HDR bracketing, bulb timer | Shoot tab entries, no `PROP_LEN` asserts. Property writes are limited to `property_whitelist.h` (shutter, aperture, ISO, exposure compensation, drive, remote SW1/SW2): `_prop_request_change` (0xFE104D60) is PropertyMgr's request-change entry. Not exercised: no picture is taken in emulation |
| file_man, img_name, Lua | loaded at boot: `[MODULE] loaded <name>` required by the scenario (`--expect-modules`), Modules tab shows OK, Lua adds its Scripts tab. The build job checks every shipped module's imports against `t100_110.sym` (`tools/check_module_syms.py`): Lua needed `wait_lv_frames`, which only existed with the state object hooks, and its failed link had made every module show "Err" |
| sd_clock | shipped, off by default |
| Power save | photo build idles at Canon counter 0 (RAM 0x3164C, all samples), ML holds 0 at idle and after the menu; `ml-full` keeps the counter at 1 |

### Next steps
1. LiveView: the YUV buffer addresses are allocated at run time (`docs/firmware/4000D-consts.md`); log
   `YUV422_LV_BUFFER_DISPLAY_ADDR` in LiveView (emulator, if its LiveView path runs, or the camera) before
   enabling `CONFIG_LIVEVIEW` and the overlay features.
2. Deep sleep model for the 4000D in QEMU-EOS-Optimized (clock gate `0xC0400008` bit 1, self-refresh callback), to
   run the display-off checks the 1100D has.
3. A shutter path in the emulator (or the camera) to exercise bracketing / bulb / intervalometer shots.
4. Missing stubs (audio API, `ptpPropSetUILock`) only if a feature needs them.

### Useful captures from the real camera (read-only, logging to the card)
- MPU message log at boot and while pressing every button (replaces the borrowed 1200D/600D tables in QEMU).
- DryosDebugMsg log (`GUI_Control:%d` lines) while pressing every button: confirms `gui.h` on hardware.
- A RAM dump of `0x351A4..0x35260` with and without half-shutter pressed (HALFSHUTTER_PRESSED), and of the
  display struct `0x31010..0x310C0` with the screen on and off (DISPLAY_IS_ON).
- Card LED: whether `0xC022D1E0` with `0x93D800` / `0x83DC00` lights it (needs one write; only with consent).

## Steps
| # | Step | Verified by |
|---|---|---|
| 1 | **Emulator model** for 4000D in QEMU-EOS-Optimized (derive from 1300D: memory map, MPU spells, display, SD) | Canon firmware boots to GUI in QEMU; screenshot |
| 2 | **Firmware RE** with Ghidra: locate every stub listed in `platform/1300D.110/stubs.S` in the 4000D ROM (string/xref/signature matching vs 1300D patterns), plus consts (`consts.h`: VRAM, LV buffers, property IDs, GUI events) | Each stub has address + evidence in `docs/firmware/4000D-stubs.md`; automated check script compares stub signatures |
| 3 | **Minimal boot** (`minimal` / hello-world target) | Runs in QEMU, prints on screen |
| 4 | **Platform dir** `platform/4000D.110` (stubs.S, consts.h, internals.h, features.h minimal) | Builds with FATAL_WARNINGS=y; boots ML menu in QEMU |
| 5 | **Features** enabled incrementally (photo profile first: overlays, intervalometer, bracketing, powersave hold) | CI emulation tests per feature |
| 6 | **Boot flag**: no signed installer is needed. Canon's own `EnableBootDisk` routine, present in the 4000D ROM, is invoked over USB with MLinstall (GPL-2.0, github.com/petabyt/mlinstall), as on the 1300D; reported working on a 4000D fw 1.0.1. The flags are the first 16 bytes of ROM1 (FIR, BOOT, RAM, UPD; currently 0, 0, -1, -1). First emulate the flag write in QEMU to see exactly which flash range Canon erases and rewrites | QEMU trace of the flash write; then on hardware with the maintainer: read-only device info, one `EnableBootDisk` (full battery, menu not LiveView, no card I/O), dumper re-run shows BOOT set |
| 7 | **Hardware** (last, maintainer): read-only smoke test first, then features | Maintainer sign-off |

## Risks
- Enabling the camera boot flag is the only step that writes to the camera (one Canon flash-block rewrite). Low but
  non-zero risk: a power loss during that write could corrupt the block; never in LiveView or during sensor cleaning.
  It is reversible over USB (`DisableBootDisk`). A bad build on a bootable card only hangs at power-on (remove battery
  and card to recover); it does not brick the camera.
- Stubs found by pattern matching can be wrong in subtle ways; each must be validated in emulation before use.
