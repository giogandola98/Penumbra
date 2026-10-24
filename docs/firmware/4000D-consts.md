# 4000D 1.1.0: LiveView, movie and power-save constants

`platform/4000D.110/consts.h` started from the 1300D values for the LiveView / movie / raw code paths, which this
port does not enable yet. This page records which of them were located in the 4000D firmware (static analysis of
the maintainer's own ROM1 dump) and how. Addresses only; the descriptions are ours.

Method: find the constant's use in the 1100D 1.0.5 firmware (a DIGIC 4 camera whose ML constants work), identify
that function by its debug strings or asserts, find the 4000D function with the same strings, and read the literal
or field offset it uses. None of the 1300D literals (`0x40D07800`, `0x44000080`, ...) occur in the 4000D ROM; that
alone proves nothing, since the 1100D's working buffer values are not ROM literals either.

Status: **verified** = read from the 4000D code as described; **likely** = same structure as on the 1100D but the
exact field was not seen in use; **not verified** = still the 1300D value.

## LiveView / display

| Constant | consts.h | Evidence | Status |
|---|---|---|---|
| `REG_EDMAC_WRITE_LV_ADDR` | `0xC0F04308` | the LV path connects EDMAC write channel 3; its completion callback (0xFE58F9C8) logs `WriteEDmacVramCompleteCBR`. Channel table at 0xFE86F280: channel 3 = `0xC0F04300`, +8 = address register. Same as the 1100D | verified |
| `REG_EDMAC_WRITE_HD_ADDR` | `0xC0F04208` | write channel 2, callback 0xFE58FA08 logs `WriteEDmacYuvCompleteCBR`; channel 2 = `0xC0F04200` | verified |
| `YUV422_LV_BUFFER_DISPLAY_ADDR` | `*(0x31010 + 0xB0)` | `SetImageVramAddress` (0xFE10C304) logs this field as the image VRAM address (1100D: `0x238C + 0xAC`, its ML value) | verified |
| `YUV422_LV_BUFFER_1..3` | 1300D values | allocated at run time by the SRM allocator (`SRM_AllocateMemoryResourceForLiveViewYuv`, 0xFE0D51FC), not ROM literals | **not verified**: needs a RAM dump or a log of the display address in LiveView |
| `YUV422_HD_BUFFER_1..5` | 1300D values | same run-time allocation | **not verified** |
| BMP VRAM | 960x540 (`0x3C0` x `0x21C`, `0x7E900` bytes), 720x480 visible | `CreatePhysicalVram` (0xFE10ED8C) and `GetVramSize` (0xFE10B320) give the same sizes as on the 1100D. Emulation screenshots: 720x480, no offset, correct palette | verified |
| LV image size | 720x480 | the image-VRAM size table matches the 1100D's, whose LV buffer is 720x480 | likely |
| `FRAME_SHUTTER_BLANKING_*` | 1300D values (`0x40481B20/24`) | run-time sensor register table, not in ROM | **not verified** |
| `FOCUS_CONFIRMATION` | 0 | the 1100D field belongs to a struct whose layout changed here (offsets moved by 4) | not located |

## GUI modes and LiveView state

| Constant | consts.h | Evidence | Status |
|---|---|---|---|
| `CURRENT_GUI_MODE` | `*(0x35D08 + 0x48)` | the GUI-mode setter 0xFE143B94 writes +0x48, the getter 0xFE143E08 reads it (1100D `0x3918 + 0x48`) | verified |
| `GUIMODE_WB` / `PICTURE_STYLE` / `Q_UNAVI` / `FLASH_AE` / `PICQ` | `0x2B` / `0x2A` / `0x23` / `0x28` / `0x2D` (were 5 / 4 / 0x1F / 0x22 / 6) | values passed to the setter by the IDLE handler (0xFE2BD4D8..0xFE2C2EA8), named by its `START_*` strings (e.g. `START_UNAVI_WB_MODE` = 0x2B). The numbering equals the 1100D's up to 0x46. The old 1300D values are the menu-dialog modes of the same settings, not the Q-screen ones | verified (not used by ML's code today) |
| `GUIMODE_PLAY` / `MENU` / `FOCUS_MODE` / `DRIVE_MODE`, movie 0x1E / 0x1F | 1 / 2 / 9 / 8 / 0x1E / 0x1F | same table (`START_MENU_AF_MODE` = 9, `START_WARNING_LENSLESS_MOVIE_MODE` = 0x1E, `MISC_MOVIE` = 0x1F) | verified |
| `GUIMODE_ML_MENU` | 2 | in LiveView it would be 0x44 (`START_LV_PS_SETTING_MODE`, the 1100D's 68) | verified |
| `ISO_ADJUSTMENT_ACTIVE` | 0 (found: `*(int *)0x37A9C == 0xF`) | field written by `ptpNotifyOlcInfoChanged` (0xFE1DC930); 1100D `0x5334 + 0x1C` | verified, kept 0 until LiveView |
| `LV_BOTTOM_BAR_DISPLAYED` | 0 (found: `*(int8_t *)0x37A9C == 0xF \|\| *(int8_t *)0x3F564 != 0x17`) | the field above and the UNAVI feedback field already used by `UNAVI_FEEDBACK_TIMER_ACTIVE` | likely, kept 0 until LiveView |
| `AE_STATE` / `AE_VALUE` | 0 (found: `0x3A1C8 + 0x1C` / `+ 0x1D`) | `copyOlcDataToStorage` (0xFE2A89E8) storage at 0x3A1C8 (1100D 0x7E18) | AE_STATE verified, AE_VALUE likely; kept 0 until LiveView |
| `LVAE_STRUCT` | `0x3AF2C` (was 0x3B7A4) | `lvae_setdispgain` / `setcontrolbv` / `setmoviemanualcontrol` (0xFE321FCC / 0xFE322638 / 0xFE32266C) write +0x26 / +0x1C / +0x78; the control-parameter setter (0xFE322404) writes +0x1E..+0x24. Offsets as on the 1100D (0x8B7C) | verified |
| `VIDEO_PARAMETERS_SRC_3` | `0x68590` (was 0x6A95C) | `[EVF] evfSetParamInterrupt` reads byte +8, byte +9, half +0xE of the struct returned by 0xFE3019C0 (1100D 0x70C0C); +0xA / +0xC not seen | base verified, `FRAME_SHUTTER` / `FRAME_SHUTTER_TIMER` fields likely |
| `SHOOTING_MODE` | 1300D value | not used by ML's code | not located |

## Movie, raw, playback

| Constant | consts.h | Evidence | Status |
|---|---|---|---|
| `MVR_992_STRUCT` | `*(0x30DD4 + 4)` (was 0x315DC + 4) | `mvrResultCBR` uses this pointer (1100D `0x1DF0 + 4`); fields 0x14C / 0x128 are loaded and stored in `mvrSMEncodeDone` where the 1100D does | base verified, offsets likely |
| `DEFAULT_RAW_BUFFER` | `MEM(MEM(0x370C0 + 0x30))` (was 0x37930) | getter 0xFE194E5C, same code as the 1100D's (0xFF0F7434, `0x4C34 + 0x30`) | verified |
| `IMGPLAY_ZOOM_POS_X/Y` | `MEM(0x6D8F8)` / `+4` (was 0x6FCC4) | `[ImgPlyer] CentrePos x:%ld y:%ld` prints these two words | address verified; centre / delta values not verified |

## Power save (for the emulation and the powersave holds)

| Item | Address | Evidence | Status |
|---|---|---|---|
| `DisablePowerSave` / `EnablePowerSave` | 0xFE11A288 / 0xFE11A2F4 | eventprocs; `./PowerMgr/PowerSaver.c` asserts (`PowerSaveCounter < MAX_COUNTER`, max 32, and `> 0`), log `[PM] Disable/EnablePowerSave (Counter = %d)` | verified |
| PowerSaver state | RAM 0x31648: +0 magic (pointer to "PowerSaver"), **+4 counter (0x3164C)** | literal 0xFE11A37C of both eventprocs; same layout as the 1100D's 0x2B60 | verified (emulation: RAM samples equal the `[PM]` log) |
| PowerMgr state | RAM 0x39650: +0 running, +4 param, +8 mode (1 = WFI at full clock, 2 = power save), +0xC "not in self-refresh", +0x10 self-refresh callback, +0x14 last-sleep stamp | `PowerMgr_task_entry` 0xFE289544, `PowerMgr_set_mode` 0xFE289684 (literal 0xFE2896D8) | verified |
| Deep-sleep condition | mode 2, bit 1 of `0xC0400008` clear (image engine unclocked), more than 1000 ticks since the stamp: `[PM] pmSelfRefresh : In/Out` around the callback | same code shape as the 1100D | verified (static) |

Deep sleep is modelled in QEMU-EOS-Optimized for the 4000D too (clock gate in `hw/eos/cams/pm_4000d.c`);
the 4000D's `selfRefresh` and its registers are in `4000D-powersave.md`.
