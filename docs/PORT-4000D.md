# Port: Canon EOS 4000D / Rebel T100 (fw 1.1.0)

Status: **planned** — starts after the development chain is complete on the 1100D (emulation, PM modeling,
powersave hold verified). Maintainer's own dump: ROM1 valid (MD5 matches the on-camera dumper), ROM0 is an
unpopulated region (repeated 16-byte bus pattern). No serial flash. Model ID 0x422, DryOS 2.3 r58+p7, ROMBASEADDR
0xFF0C0000. ROMs: private package registry, package `canon-rom`, version `4000D-110` (never in this repository).

Base platform: `1300D.110` (DIGIC 4+, armv5te, `boot-d45-ch.o`, same DryOS generation). Any upstream/community 4000D
work (forum threads, qemu-eos `2000d_test` branch) is checked first and reused only if the license allows (GPL-2.0).

## Progress (branch `port/4000d`)

| # | Status | Notes |
|---|---|---|
| 1 | done (QEMU-EOS-Optimized) | Canon GUI and menu in emulation; see that repo's `docs/models/4000D.md` |
| 2 | done | 161 of 167 stubs located (152 high, 9 medium confidence), 6 audio/PTP ones missing and not needed by the minimal build; table and method in `firmware/4000D-stubs.md`; `tools/check_stubs.py` re-checks every stub against the ROM in the emulation job |
| 3 | merged into 4 | the normal build is used directly (QEMU scenario `ml-full`) instead of a separate hello-world target |
| 4 | boots in QEMU | `platform/4000D.110` builds with `FATAL_WARNINGS=y` (CI build job). CI run 56 (emulation job): stub check 161/161 OK; `ml-full` and `ml-hold` boot ML (ml_init, menu_task, shoot_task ... in the task log; `[PS] hold boot`, `hold menu` / `release menu` when Delete opens and MENU closes the ML menu; `menu.png` in the artifact); no QEMU errors. Not yet reviewed by eye: the screenshots |
| 5-7 | not started | |

Platform facts used by step 4 (evidence in `firmware/4000D-stubs.md`): main firmware `0xFE0C0000`, kernel copied
to RAM `0x1900..0x4D0AC` by cstart, BSS to `0x829D4`, Canon heap `0x2D0000..0xD00000` (ML takes
`0xC80000..0xD00000` through the cache hack at `0xFE0C1B74`, as on the 1300D), init_task literal at `0xFE0C3A9C`,
firmware signature `0x1B6EF034`, firmware ID `0x80000422`, task structs as `CONFIG_TASK_STRUCT_V2` /
`CONFIG_TASK_ATTR_STRUCT_V4` (read off `_get_task_info_by_id`), new-style task dispatch hook. Display: the BMP
overlay is the usual DIGIC 4 720x480 (`CONFIG_4_3_SCREEN`), scaled by the hardware to the 2.7" 230k-dot panel.

Minimal feature set (`features.h`): no LiveView, movie, raw, FPS or audio; debug menus (tasks, GUI events, free
memory, screenshot) only. Constants of those code paths that could not be located yet are marked
"1300D value, not verified" in `consts.h` and must be checked before the feature is enabled.

Buttons: the 4000D shares the 1200D/1300D layout (Av and Erase on one key), so `gui.h` starts from the 1300D
codes; the `canon` emulation scenario sends each emulated key once and logs the GUI events Canon derives from it
(`keys.json`), which checks them. Run 56 confirmed every code in `gui.h` that the emulator can send: MENU 6,
INFO 7, PLAY 0xB, Erase 0xD, Q 0x1C, SET 4, up/down/left/right 0x27/0x29/0x25/0x23, zoom in/out 0xE/0x10, and Av
as event 0x61 (`GMT_OLC_INFO_CHANGED`, used by `CONFIG_MENU_WITH_AV`). The emulator's key mapping is borrowed
from the 1200D, so the real camera's MPU codes still need one check (capture below).

Idle in emulation (run 56, relative only): canon 213 WFI/s, 86 task switches/s; ml-full 254 switches/s
(`audio_common_task` ~50/s although audio is off: candidate for the photo profile).

### Next steps
1. Look at `boot.png` / `menu.png` / `key*.png` of the 4000D emulation artifact; then the photo profile
   (`ML_PROFILE`, `POWERSAVE_HOLD`) for 4000D as for EOSM, and features one by one (step 5).
2. Locate the LiveView/movie constants still marked "1300D value, not verified" before enabling LiveView.
3. Missing stubs (audio API, `ptpPropSetUILock`) only if a feature needs them.

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
