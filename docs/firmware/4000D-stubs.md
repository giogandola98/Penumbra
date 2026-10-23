# 4000D 1.1.0 stubs: addresses and evidence

`platform/4000D.110/stubs.S` keeps the layout of `platform/1300D.110/stubs.S` (the template, same DIGIC 4+
platform and DryOS r58 generation). Every 1300D stub was looked up again in the 4000D ROM; no 1300D address
was copied. The table below lists each one with the evidence used and a confidence level.

Totals: **152 high, 9 medium, 6 missing** (of 167 stubs; the 11 Wi-Fi/socket entries were already disabled on
the 1300D and are not ported).

## Memory layout used

| Item | Value | Evidence |
|---|---|---|
| ROM1 | 16 MiB at `0xFE000000` (mirrored up to `0xFFFFFFFF`) | the 32 MiB dump holds the same 16 MiB twice; firmware pointers use the `0xFE` alias |
| Main firmware | `0xFE0C0000` | `B 0xFE0C000C` at the entry, as on the 1300D; ROMBASEADDR `0xFF0C0000` is the same code via the mirror |
| Kernel copy | ROM `0xFE9A15FC` -> RAM `0x1900..0x4D0AC`, BSS to `0x829D4` | literals of the copy and clear loops at `0xFE0C00DC..E8` |
| Canon heap | `0x2D0000..0xD00000` | `MOV R0,#0x2D0000` / `MOV R1,#0xD00000` at `0xFE0C1B74`; ML lowers the end to `0xC80000` (RESTARTSTART) |
| Firmware signature | `0x1B6EF034` (`SIG_4000D_110`) | sum of the 0x10000 words from `0xFE0C0000` (same method gives the known 1100D value) |
| Model / firmware ID | `0x422` / `0x80000422` | "Canon EOS Rebel T100" record at `0xFE040044` |

## How the stubs were found

- **debug string / assert**: the function prints a DryosDebugMsg format string, or passes a source file name
  and line to the assert handler (DryOS kernel: `SystemIF::KerSem.c`, `KerQueue.c`, `KerRLock.c`, ...).
- **1100D match**: the 1100D 1.0.5 function of the same name (from its ML stubs) compared with every 4000D
  function near the address predicted from the 1300D stub. Instructions are compared with branch offsets and
  literal addresses masked, string literals compared by content; the number is the similarity (0..1). A
  match is high when the best candidate is clearly ahead and its position agrees with the 1300D neighbours.
- **call graph**: position in a caller/callee chain that is already identified (e.g. the call right after
  `_prop_request_change` in CreateSkeltonJob, as on the 1100D).
- **byte pattern**: same instruction sequence as the 1100D function, apart from addresses.
- **task name**: entry point passed to `task_create("Name", ...)`.

Within a code region the 4000D addresses keep a constant offset from the 1300D ones (e.g. EDMAC -0x276D0,
GUI -0xB934, kernel RAM -0x74 / -0x300); a stub whose offset disagrees with its neighbours was re-checked.

## Automatic check

`tools/check_stubs.py platform/4000D.110 ROM1.BIN` re-checks every stub against the evidence written in
`platform/4000D.110/stubs-evidence.txt` (referenced strings, assert file/line, calls and callers, literals,
call-site counts). The emulation job runs it with the CI ROMs before the scenarios; without a ROM it prints
`SKIPPED`. Swapping two FIO stubs or moving a stub by one instruction makes it fail.

## Table

| Stub | Address | Evidence | Confidence | Notes |
|---|---|---|---|---|
| `cstart` | `0xFE0C39A0` | call graph | high | target of the B at 0xFE0C0638 after the ROM->RAM copy loop; calls bzero32 and create_init_task |
| `bzero32` | `0x290A4` | call graph | high | first BL in cstart (r1 = 0x78); RAM copy of the kernel |
| `create_init_task` | `0x51E0` | call graph | high | last BL in cstart, r1 = init_task |
| `init_task` | `0xFE11F190` | call graph | high | literal at 0xFE0C3A9C passed by cstart to create_init_task (HIJACK_CACHE_HACK_INITTASK_ADDR) |
| `additional_version` | `0x64E1C` | 1100D match | high | GUI_GetFirmVersion (0xFE2B64DC) aligned with the 1100D one: literal 0x833C -> 0x64E1C |
| `_FIO_CloseFile` | `0xFE27C718` | 1100D match | high | 0.97 shape match; FIO block keeps 1300D spacing (Open +0x460) |
| `_FIO_FindClose` | `0xFE27D374` | 1100D match | high | 0.97 shape match |
| `_FIO_FindNextEx` | `0xFE27D294` | 1100D match | high | 0.98 shape match |
| `_FIO_ReadFile` | `0xFE27C4C8` | 1100D match | high | 0.86 match; called by FR_SyncReadFile |
| `_FIO_SeekSkipFile` | `0xFE27C578` | 1100D match | high | same size as 1100D (208), 1300D spacing Read+0xB0, called by FR_SyncReadFile |
| `_FIO_WriteFile` | `0xFE27C668` | 1100D match | high | 0.86 match; called by FW_SyncWriteFileFromBuffer |
| `_FIO_CreateDirectory` | `0xFE27CEF0` | call graph | high | called by FIO_CreateDirectory and FIO_SetFileAttributes, as on 1100D |
| `_FIO_CreateFile` | `0xFE27C374` | 1100D match | high | 0.98 shape match, Open+0xBC as on 1300D |
| `_FIO_FindFirstEx` | `0xFE27D1A0` | 1100D match | high | 0.87 match; called by FM_GetNumOfDCIMInExtDrive |
| `_FIO_GetFileSize` | `0xFE27C894` | 1100D match | high | 1.00 shape match |
| `_FIO_OpenFile` | `0xFE27C2B8` | 1100D match | high | 0.85 match; first of the FIO block |
| `_FIO_RemoveFile` | `0xFE27C420` | 1100D match | high | 0.90 match; called by FIO_RemoveFile |
| `_FIO_RenameFile` | `0xFE27CD28` | 1100D match | high | 0.91 shape match |
| `gui_main_struct` | `0x30B74` | call graph | high | struct read by gui_main_task (queue at +0x30, counter at +0x0C) |
| `gui_task_list` | `0x3D1A0` | 1100D match | high | RAM literal in the dialog module; dialog_redraw reads gui_task_list+0x1C (0x3D1BC) |
| `GUI_ChangeMode` | `0xFE0CC434` | debug string | high | "GUI_ChangeMode" DryosDebugMsg prefix |
| `GUI_Control` | `0xFE0CC278` | debug string | high | "GUI_Control:%d 0x%x" |
| `SetGUIRequestMode` | `0xFE143E2C` | 1100D match | high | 0.83 match; string signature of 1100D stub |
| `gui_init_end` | `0xFE0CC8E8` | debug string | high | "< GUI Lock > ERR EndGuiInit"; first call of gui_main_task |
| `gui_main_task` | `0xFE0CBEA8` | task name | high | task_create("GuiMainTask") entry |
| `gui_massive_event_loop` | `0xFE145ECC` | call graph | high | called by GUI_CONTROL; "gui control end", GUICMD_* strings (GUI_Control_Post) |
| `GUI_SetLvMode` | `0xFE2B2E10` | debug string | high | "GUI_SetLvMode" DryosDebugMsg prefix |
| `Gui_SetSoundRecord` | `0xFE2B2FD0` | debug string | high | "GUI_SetSoundRec"; sets property 0x0205000E (PROP_MOVIE_SOUND_RECORD) |
| `PowerMicAmp` | `0xFE11572C` | debug string | medium | "PowerControlMicAmp(%d,%d)" (name differs from older bodies) |
| `SetAudioVolumeOut` | `0xFE113C6C` | assert | medium | ./Audio/AudioIC.c, "Keep Volume (%d->%d)" |
| `SetNextASIFADCBuffer` | `0xFE100374` | debug string | high | "SetNextINT16ASIFADCBuffer" |
| `SetNextASIFDACBuffer` | `0xFE10055C` | debug string | high | "SetNextINT16ASIFDACBuffer" |
| `SetSamplingRate` | `0xFE114968` | assert | high | ./Audio/AudioIC.c, "SamplingRate Change(%d>%d) > PLL Reboot" |
| `StartASIFDMAADC` | `0xFE0FF874` | debug string | high | "StartASIFDMAADC" |
| `StartASIFDMADAC` | `0xFE0FFC34` | debug string | high | "StartASIFDMADAC" |
| `StopASIFDMADAC` | `0xFE0FFEA8` | debug string | high | "StopASIFDMADAC" |
| `SoundDevActiveIn` | `0xFE101AB4` | debug string | high | "SoundDevActiveIn" |
| `StopASIFDMAADC` | `0xFE0FFB0C` | debug string | high | "StopASIFDMAADC" |
| `sounddev` | `0x30DFC` | 1100D match | high | sounddev_task aligned with the 1100D one: literal 0x1E58 -> 0x30DFC |
| `audio_thresholds` | `0xFE866FA8` | byte pattern | high | 41-entry table 0x7FFF, 0x7213, 0x65AB, ... identical to 1100D, referenced next to "AudioLevel" (ALV_Initialize) |
| `sounddev_active_in` | `0xFE101AB4` | debug string | high | "SoundDevActiveIn" |
| `sounddev_task` | `0xFE1016A4` | task name | high | task_create("SoundDevice") entry, "[SND] _SoundDeviceTask:%1X" |
| `LCD_Palette` | `0x52640` | 1100D match | high | loaded right after 0xC0F14400 in the bitmap display init (0xFE10FEA0), 1300D 0x54A40 - 0x2400 like bmp_vram_info |
| `bmp_vram_info` | `0x53E40` | 1100D match | high | TestBMP (0xFE10F21C) aligned with 1100D TestBMP: 0x1D86C -> 0x53E40 |
| `GetCFnData` | `0xFE69991C` | 1100D match | high | 1.00 shape match |
| `SetCFnData` | `0xFE699B48` | 1100D match | high | 0.98 shape match |
| `camera_engine` | `0xC0220000` | other | high | MMIO base, same on all DIGIC 4 |
| `_mpu_send` | `0xFE272234` | assert | high | "dwSize < TXBD_DATA_SIZE" |
| `_mpu_recv` | `0xFE10849C` | call graph | medium | passed as last argument by the Intercom init wrapper (0xFE1085E4) to 0xFE272180; dispatches MPU messages by class |
| `dma_memcpy` | `0xFE11A90C` | 1100D match | high | string signature of 1100D stub |
| `dm_names` | `0x313F8` | 1100D match | high | second table literal in DryosDebugMsg, as 0x28E4 on 1100D |
| `DryosDebugMsg` | `0xFE117178` | call graph | high | 593 call sites preceded by a format string; also used by the QEMU model |
| `dm_set_store_level` | `0xFE1174DC` | 1100D match | high | same code as 1100D: writes level to dm struct +20 (the field DryosDebugMsg compares) |
| `dm_set_print_level` | `0xFE117474` | 1100D match | high | twin of dm_set_store_level writing +21 |
| `dialog_redraw` | `0xFE3A3554` | assert | high | ./Dialog/Dialog.c line 728 "pDialog != NULL", same code as 1100D (line 732); 366 call sites |
| `dialog_set_property_str` | `0xFE2BA174` | 1100D match | high | same code: strlen + set dialog property type 10 |
| `ErrCardForLVApp_handler` | `0xFE52D5A0` | 1100D match | high | string signature of 1100D stub |
| `ErrForCamera_handler` | `0xFE3FA314` | 1100D match | high | string signature of 1100D stub (0.77) |
| `HideUnaviFeedBack_maybe` | `0xFE421C04` | byte pattern | high | same 8 instructions as 1100D (struct 0x3F550 +0x14 = 0x17, GUI post 2/0x36) |
| `LiveViewApp_handler` | `0xFE4267B0` | debug string | high | "DlgLiveView.c FeedBackCancel(%#x)", "LOCAL_DIALOG_REFRESH_LV" |
| `LiveViewApp_handler_BL_JudgeBottomInfoDispTimerState` | `0xFE426E1C` | call graph | high | the only BL to JudgeBottomInfoDispTimerState (0xFE423970) inside LiveViewApp_handler |
| `LiveViewApp_handler_end` | `0xFE428458` | debug string | high | "StartLiveViewApp(%#x)" (function after the handler) |
| `PlayMain_handler` | `0xFE4421A0` | debug string | high | "DlgPlayMain.c CrossKeyEvent Event(0x%x)" etc. |
| `PlayMovieGuideApp_handler` | `0xFE568974` | 1100D match | high | string signature of 1100D stub |
| `ShootOlcApp_handler` | `0xFE43595C` | 1100D match | high | string signature of 1100D stub (0.73) |
| `MirrorDisplay` | `0xFE3C0974` | call graph | high | "-->Mirror start" debug msg + BL MirrorTFT (eventproc 0xFE110F1C) |
| `NormalDisplay` | `0xFE3C09D4` | call graph | high | BL NormalTFT (eventproc 0xFE110F44) |
| `ReverseDisplay` | `0xFE3C09A4` | call graph | high | BL ReverseTFT (eventproc 0xFE110EF4) |
| `AbortEDmac` | `0xFE2771D8` | 1100D match | high | 0.67 match, EDMAC block keeps 1300D spacing (-0x276D0) |
| `ConnectReadEDmac` | `0xFE276EE8` | 1100D match | high | string signature of 1100D stub |
| `ConnectWriteEDmac` | `0xFE276E24` | 1100D match | high | string signature of 1100D stub |
| `RegisterEDmacAbortCBR` | `0xFE2772A0` | 1100D match | high | string signature of 1100D stub |
| `RegisterEDmacCompleteCBR` | `0xFE277278` | 1100D match | high | 1.00 shape match |
| `RegisterEDmacPopCBR` | `0xFE2773A4` | 1100D match | high | 1.00 shape match |
| `SetEDmac` | `0xFE276D6C` | 1100D match | high | string signature of 1100D stub |
| `StartEDmac` | `0xFE277084` | 1100D match | high | string signature of 1100D stub |
| `UnregisterEDmacAbortCBR` | `0xFE2772E0` | 1100D match | high | string signature of 1100D stub |
| `UnregisterEDmacCompleteCBR` | `0xFE27728C` | 1100D match | high | 1.00 shape match |
| `UnregisterEDmacPopCBR` | `0xFE2773B8` | 1100D match | high | 1.00 shape match |
| `GUI_SetRollingPitchingLevelStatus` | `0xFE2B2928` | debug string | high | "GUI_SetRollingPitchingLevelStatus Status(%d)" |
| `_EngDrvOut` | `0xFE27816C` | 1100D match | high | string signature of 1100D stub |
| `_engio_write` | `0xFE278518` | 1100D match | high | string signature of 1100D stub |
| `shamem_read` | `0xFE27805C` | 1100D match | high | 0.86 match, 1300D spacing to _EngDrvOut |
| `call` | `0xFE117D60` | 1100D match | high | 0.90 shape match (eventproc dispatcher) |
| `TryPostEvent` | `0xFE28EDA0` | call graph | high | TaskClass post (checks "TaskClass" signature); called before "GetFileCacheItem : TryPostEvent" error message |
| `TryPostEvent_end` | `0xFE28EE00` | call graph | high | start of the next function |
| `TryPostStageEvent` | `0xFE28BFF0` | byte pattern | high | same code as TryPostEvent, checks the "StageClass" signature |
| `TryPostStageEvent_end` | `0xFE28C050` | call graph | high | start of the next function |
| `AddMemoryChunk` | `0xFE119B98` | 1100D match | medium | 0.68 match; ExMem block order as on 1300D |
| `AllocateContinuousMemoryResource` | `0xFE11AFFC` | 1100D match | high | 0.68 match; 1300D spacing (AllocateMemoryResource +0x4C) |
| `AllocateMemoryResource` | `0xFE11AFB0` | 1100D match | high | 0.68 match; 1300D spacing |
| `CreateMemoryChunk` | `0xFE119334` | 1100D match | high | 0.72 match |
| `CreateMemorySuite` | `0xFE119940` | 1100D match | high | 0.77 match |
| `DeleteMemorySuite` | `0xFE1197B4` | 1100D match | medium | 0.39 match; GetMemoryAddressOfMemoryChunk +0x74 as on 1300D |
| `FreeMemoryResource` | `0xFE11B114` | 1100D match | high | 0.76 match; 1300D spacing |
| `GetFirstChunkFromSuite` | `0xFE119C68` | call graph | high | 0.81 match; aligned call in CreateSkeltonJob vs 1100D |
| `GetMemoryAddressOfMemoryChunk` | `0xFE119740` | call graph | high | 0.67 match; aligned call in CreateSkeltonJob vs 1100D |
| `GetNextMemoryChunk` | `0xFE11A010` | 1100D match | medium | 0.54 match |
| `SRM_AllocateMemoryResourceFor1stJob` | `0xFE0D494C` | call graph | high | call after _prop_request_change in CreateSkeltonJob, as on 1100D; "pPriorMessage" assert |
| `SRM_FreeMemoryResourceFor1stJob` | `0xFE0D6F14` | call graph | high | 2 callers at the same places as on 1100D; "pPriorMessage" assert |
| `gui_timer_struct` | `0x364AC` | call graph | high | RAM literal in GUI_Control_Post (1300D 0x36CAC - 0x800) |
| `gui_change_mode` | `0xFE147D6C` | debug string | high | "GUI_ChangeMode_Post:%d, %d, %d" |
| `gui_local_post` | `0xFE14854C` | byte pattern | high | same switch dispatcher as 1100D gui_local_post; 1300D spacing to gui_change_mode |
| `gui_other_post` | `0xFE148B74` | 1100D match | high | string signature of 1100D stub |
| `gui_change_shoot_type_post` | `0xFE148F5C` | debug string | high | "GUI_ChangeShootType_Post" |
| `gui_init_event` | `0xFE1497E8` | debug string | high | "-> handleGuiInit", "AllocateBmpVram" |
| `CancelDateTimer` | `0xFE41C7C8` | 1100D match | high | string signature of 1100D stub |
| `CancelTimer` | `0xFE11EB40` | 1100D match | high | 0.81 match; called where 1100D ErrForCamera_handler calls CancelTimer |
| `SetHPTimerAfterNow` | `0xFE118480` | 1100D match | high | 0.72 match |
| `SetHPTimerNextTick` | `0xFE118574` | byte pattern | high | same code as 1100D (expiry = r0 + r1) |
| `SetTimerAfter` | `0xFE11EA70` | 1100D match | high | 0.54 match; CancelTimer - 0xD0 as on 1300D |
| `mvr_config` | `0x38250` | call graph | high | struct written by mvrSetFullHDOptSize/mvrSetDefQScale |
| `mvrFixQScale` | `0xFE25BFA4` | byte pattern | high | same code as 1100D (mvr_config +4 / +22) |
| `mvrSetDefQScale` | `0xFE25BA68` | byte pattern | high | same code as 1100D (mvr_config +10) |
| `mvrSetFullHDOptSize` | `0xFE25BAB0` | 1100D match | high | string signature of 1100D stub |
| `mvrSetGopOptSizeFULLHD` | `0xFE25BC94` | 1100D match | high | string signature of 1100D stub |
| `post_isr_hook` | `0x308AC` | call graph | high | called after the ISR in the IRQ handler (ROM 0xFE0C0738) |
| `pre_isr_hook` | `0x308A8` | call graph | high | called before the ISR in the IRQ handler (ROM 0xFE0C06DC) |
| `DispSensorStart` | `0xFE121678` | debug string | high | "req DispSensorStart %d %d %d %d %d ( %d %d )" |
| `cf_device` | `0x485C8` | 1100D match | medium | RAM literal of the CF driver match; sd_device - 0x24 as on 1300D |
| `sd_device` | `0x485EC` | 1100D match | high | SD_SetBusWidth-like function aligned with 1100D (0.85): 0x15560 -> 0x485EC |
| `fsuDecodePartitionTable` | `0xFE4C5EAC` | 1100D match | high | 0.78 shape match |
| `GetMemoryInformation` | `0xFE11D574` | 1100D match | high | _AllocateMemory - 0x260 as on 1300D |
| `GetSizeOfMaxRegion` | `0xFE11D520` | 1100D match | high | 0.75 match, _AllocateMemory - 0x2B4 as on 1300D |
| `_AllocateMemory` | `0xFE11D7D4` | 1100D match | high | 0.76 match; called by _alloc_dma_memory |
| `_FreeMemory` | `0xFE11DB70` | 1100D match | high | 0.75 match; called by _free_dma_memory |
| `_alloc_dma_memory` | `0xFE28EEC4` | call graph | high | _AllocateMemory + cache line alignment, returns pointer \| 0x40000000 |
| `_free` | `0xFE2935A0` | 1100D match | high | 0.89 match, _malloc + 0x58 as on 1300D |
| `_free_dma_memory` | `0xFE28EF00` | call graph | high | clears 0x40000000, tail-calls _FreeMemory |
| `_malloc` | `0xFE293548` | 1100D match | high | 1.00 shape match |
| `msg_queue_count` | `0x7804` | assert | high | SystemIF::KerQueue.c (line 561), 1300D - 0x64 |
| `msg_queue_create` | `0x741C` | assert | high | SystemIF::KerQueue.c (line 95), 1300D - 0x64 |
| `msg_queue_post` | `0x77B0` | assert | high | SystemIF::KerQueue.c (line 532), 0.86 match |
| `msg_queue_receive` | `0x756C` | assert | high | SystemIF::KerQueue.c (lines 307-329), gui_main_task waits on it |
| `ptp_register_handler` | `0xFE32BB10` | 1100D match | high | string signature of 1100D stub |
| `_prop_cleanup` | `0xFE105314` | 1100D match | high | 0.89 match |
| `_prop_request_change` | `0xFE104D60` | 1100D match | high | 0.82 match; aligned call in CreateSkeltonJob vs 1100D |
| `prop_deliver` | `0xFE1051B8` | 1100D match | high | 0.71 match |
| `prop_register_slave` | `0xFE105240` | 1100D match | high | 0.75 match, prop_deliver + 0x88 as on 1300D |
| `AcquireRecursiveLock` | `0xB9B8` | assert | high | SystemIF::KerRLock.c (lines 198-215), 1300D - 0x300 |
| `CreateRecursiveLock` | `0xB8F8` | assert | high | SystemIF::KerRLock.c (line 88), 1300D - 0x300 |
| `ReleaseRecursiveLock` | `0xBB28` | assert | high | SystemIF::KerRLock.c (lines 305-318), 1300D - 0x300 |
| `CreateResLockEntry` | `0xFE27AB2C` | 1100D match | high | string signature of 1100D stub |
| `LockEngineResources` | `0xFE27B06C` | 1100D match | high | string signature of 1100D stub |
| `UnLockEngineResources` | `0xFE27B1A4` | 1100D match | high | string signature of 1100D stub |
| `create_named_semaphore` | `0x3054` | assert | high | SystemIF::KerSem.c (line 94), 1300D - 0x74 |
| `give_semaphore` | `0x32E4` | assert | high | SystemIF::KerSem.c (line 354), 1300D - 0x74 |
| `take_semaphore` | `0x31FC` | assert | high | SystemIF::KerSem.c (lines 307-324), 0.80 match |
| `task_max` | `0x350EC` | call graph | high | loop bound of the task table init (stride 0x54) at RAM 0x3F9C |
| `_get_task_info_by_id` | `0x24C4` | call graph | high | copies task fields (entry +0xC, stack +0x1C, name +0x24, context +0x50) into the attr struct |
| `current_task` | `0x30978` | call graph | high | read by _get_task_info_by_id and task_trampoline; the QEMU model uses it too |
| `current_interrupt` | `0x640` | call graph | high | IRQ handler stores MEM(0xC0201004) here (copied block, ROM 0xFE0C0810) |
| `msleep` | `0x370C` | assert | high | SystemIF::KerSys.c line 626; same code as 1100D msleep |
| `task_dispatch_hook` | `0x35128` | call graph | high | called by the scheduler as hook(n, old, new) at RAM 0x1D20 / 0x1D80; setter at 0x5904 |
| `task_create` | `0x3888` | task name | high | called with every task name (tasks.csv); SystemIF::KerTask.c |
| `task_trampoline` | `0x4168` | call graph | high | calls current_task->entry(arg) and then the task exit path |
| `lv_path_struct` | `0x6B04C` | 1100D match | medium | RAM literal of the matched 1100D function (0.83); not used by the minimal build |
| `LoadCalendarFromRTC` | `0xFE112DA8` | 1100D match | high | string signature of 1100D stub |
| `vsnprintf` | `0xFE28A874` | 1100D match | high | 0.79 match; called by DryosDebugMsg like on 1100D |
| `EnableImagePhysicalScreenParameter` | `0xFE10D398` | debug string | high | "EnableImagePhysicalScreenParameter" prefix |
| `TryReceiveMessageQueue` | `0x767C` | assert | medium | SystemIF::KerQueue.c: non-blocking receive (returns 13 when empty) |
| `PowerAudioOutput` | - | - | missing | no matching function: the audio IC is driven through AudioICSerialCommand |
| `SetASIFMode` | - | - | missing | ASIF mode setters exist (SetASIFADCModeSingleUINT8 ...) but not this API |
| `SoundDevShutDownIn` | - | - | missing | sound device API is message based here (SoundDevStandbyIn, SoundDevStopIn) |
| `_audio_ic_read` | - | - | missing | no register read API; serial command interface instead |
| `_audio_ic_write` | - | - | missing | no register write API; serial command interface instead (AudioICSerialCommand 0xFE283C70) |
| `ptpPropSetUILock` | - | - | missing | not located yet (only used by the PTP CHDK option) |
