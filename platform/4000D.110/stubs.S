/** \file
 * Entry points into the firmware image.
 *
 * These are the functions that we can call from our tasks
 * in the Canon 1.1.0 firmware for the 4000D / Rebel T100.
 * Evidence and confidence per stub: docs/firmware/4000D-stubs.md
 *
 */
/*
 * Copyright (C) 2017 Magic Lantern Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include <stub.h>

.text

// for task information

/** Startup **/
NSTUB(MAIN_FIRMWARE_ADDR, firmware_entry)
NSTUB(0xFE0C39A0, cstart)                                  // target of the B at 0xFE0C0638 after the ROM->RAM copy loop; calls b...
NSTUB(   0x290A4, bzero32)                                 // first BL in cstart (r1 = 0x78); RAM copy of the kernel
NSTUB(    0x51E0, create_init_task)                        // last BL in cstart, r1 = init_task
NSTUB(0xFE11F190, init_task)                               // literal at 0xFE0C3A9C passed by cstart to create_init_task (HIJACK_...
NSTUB(   0x64E1C, additional_version)                      // GUI_GetFirmVersion (0xFE2B64DC) aligned with the 1100D one: literal...

/** File I/O **/
NSTUB(0xFE27C718, FIO_CloseFile)                           // 0.97 shape match; FIO block keeps 1300D spacing (Open +0x460)
NSTUB(0xFE27D374, FIO_FindClose)                           // 0.97 shape match
NSTUB(0xFE27D294, FIO_FindNextEx)                          // 0.98 shape match
NSTUB(0xFE27C4C8, _FIO_ReadFile)                           // 0.86 match; called by FR_SyncReadFile
NSTUB(0xFE27C578, FIO_SeekSkipFile)                        // same size as 1100D (208), 1300D spacing Read+0xB0, called by FR_Syn...
NSTUB(0xFE27C668, _FIO_WriteFile)                          // 0.86 match; called by FW_SyncWriteFileFromBuffer
NSTUB(0xFE27CEF0, _FIO_CreateDirectory)                    // called by FIO_CreateDirectory and FIO_SetFileAttributes, as on 1100D
NSTUB(0xFE27C374, _FIO_CreateFile)                         // 0.98 shape match, Open+0xBC as on 1300D
NSTUB(0xFE27D1A0, _FIO_FindFirstEx)                        // 0.87 match; called by FM_GetNumOfDCIMInExtDrive
NSTUB(0xFE27C894, _FIO_GetFileSize)                        // 1.00 shape match
NSTUB(0xFE27C2B8, _FIO_OpenFile)                           // 0.85 match; first of the FIO block
NSTUB(0xFE27C420, _FIO_RemoveFile)                         // 0.90 match; called by FIO_RemoveFile
NSTUB(0xFE27CD28, _FIO_RenameFile)                         // 0.91 shape match

/** GUI **/
NSTUB(   0x30B74, gui_main_struct)                         // struct read by gui_main_task (queue at +0x30, counter at +0x0C)
NSTUB(   0x3D1A0, gui_task_list)                           // RAM literal in the dialog module; dialog_redraw reads gui_task_list...
NSTUB(0xFE0CC434, GUI_ChangeMode)                          // "GUI_ChangeMode" DryosDebugMsg prefix
NSTUB(0xFE0CC278, GUI_Control)                             // "GUI_Control:%d 0x%x"
NSTUB(0xFE143E2C, SetGUIRequestMode)                       // 0.83 match; string signature of 1100D stub
NSTUB(0xFE0CC8E8, gui_init_end)                            // "< GUI Lock > ERR EndGuiInit"; first call of gui_main_task
NSTUB(0xFE0CBEA8, gui_main_task)                           // task_create("GuiMainTask") entry
NSTUB(0xFE145ECC, gui_massive_event_loop)                  // called by GUI_CONTROL; "gui control end", GUICMD_* strings (GUI_Con...
NSTUB(0xFE2B2E10, GUI_SetLvMode)                           // "GUI_SetLvMode" DryosDebugMsg prefix
// NSTUB(       ???, GUI_SetMovieSize_a)                      /* present on 50D.109 */
// NSTUB(       ???, GUI_SetMovieSize_b)                      /* present on 50D.109 */
NSTUB(0xFE2B2FD0, Gui_SetSoundRecord)                      // "GUI_SetSoundRec"; sets property 0x0205000E (PROP_MOVIE_SOUND_RECORD)

/** ASIF **/
// TODO:
// NSTUB(       ???, PowerAudioOutput)  // no matching function: the audio IC is driven through AudioICSerialCommand
NSTUB(0xFE113C6C, SetAudioVolumeOut)                       // [medium] ./Audio/AudioIC.c, "Keep Volume (%d->%d)"
NSTUB(0xFE100374, SetNextASIFADCBuffer)                    // "SetNextINT16ASIFADCBuffer"
NSTUB(0xFE10055C, SetNextASIFDACBuffer)                    // "SetNextINT16ASIFDACBuffer"
NSTUB(0xFE114968, SetSamplingRate)                         // ./Audio/AudioIC.c, "SamplingRate Change(%d>%d) > PLL Reboot"
NSTUB(0xFE0FF874, StartASIFDMAADC)                         // "StartASIFDMAADC"
NSTUB(0xFE0FFC34, StartASIFDMADAC)                         // "StartASIFDMADAC"
NSTUB(0xFE0FFEA8, StopASIFDMADAC)                          // "StopASIFDMADAC"
NSTUB(0xFE11572C, PowerMicAmp)                             // [medium] "PowerControlMicAmp(%d,%d)" (name differs from older bodies)
// NSTUB(       ???, SetASIFMode)  // ASIF mode setters exist (SetASIFADCModeSingleUINT8 ...) but not this API
// NSTUB(       ???, SetAudioVolumeIn) //?                       /* present on 6D.113 */
NSTUB(0xFE101AB4, SoundDevActiveIn)                        // "SoundDevActiveIn"
// NSTUB(       ???, SoundDevShutDownIn)  // sound device API is message based here (SoundDevStandbyIn, SoundDevStopIn)
NSTUB(0xFE0FFB0C, StopASIFDMAADC)                          // "StopASIFDMAADC"


/** Audio **/
NSTUB(   0x30DFC, sounddev)                                // sounddev_task aligned with the 1100D one: literal 0x1E58 -> 0x30DFC
// TODO: Not found direct write it looks like its some kind of serial connection now
// NSTUB(       ???, _audio_ic_read)  // no register read API; serial command interface instead
// NSTUB(       ???, _audio_ic_write)  // no register write API; serial command interface instead (AudioICSerialCommand 0xFE283C70)
NSTUB(0xFE866FA8, audio_thresholds)                        // 41-entry table 0x7FFF, 0x7213, 0x65AB, ... identical to 1100D, refe...
NSTUB(0xFE101AB4, sounddev_active_in)                      // "SoundDevActiveIn"
NSTUB(0xFE1016A4, sounddev_task)                           // task_create("SoundDevice") entry, "[SND] _SoundDeviceTask:%1X"
// NSTUB(       ???, _audio_ic_write_bulk)                     /* present on 6D.116 */

/** Bitmap **/
NSTUB(   0x52640, LCD_Palette)                             // loaded right after 0xC0F14400 in the bitmap display init (0xFE10FEA...
NSTUB(   0x53E40, bmp_vram_info)                           // TestBMP (0xFE10F21C) aligned with 1100D TestBMP: 0x1D86C -> 0x53E40

/** CFN **/
NSTUB(0xFE69991C, GetCFnData)                              // 1.00 shape match
NSTUB(0xFE699B48, SetCFnData)                              // 0.98 shape match
// NSTUB(       ???, GUI_GetCFnForTab4)                       /* present on 7D.203 */
// NSTUB(       ???, GUI_SetCFnForTab4)                    /* present on 7D.203 */

/** Camera interface? **/
// TODO:
NSTUB(0xC0220000, camera_engine)                           // MMIO base, same on all DIGIC 4

/** MPU communication **/
NSTUB(0xFE272234, _mpu_send)                               // "dwSize < TXBD_DATA_SIZE"
NSTUB(0xFE10849C, _mpu_recv)                               // [medium] passed as last argument by the Intercom init wrapper (0xFE1085E4) t...

/** DMA **/
NSTUB(0xFE11A90C, dma_memcpy)                              // string signature of 1100D stub

/** Debug messages **/
NSTUB(   0x313F8, dm_names)                                // second table literal in DryosDebugMsg, as 0x28E4 on 1100D
NSTUB(0xFE117178, DryosDebugMsg)                           // 593 call sites preceded by a format string; also used by the QEMU m...
NSTUB(0xFE1174DC, dm_set_store_level)                      // same code as 1100D: writes level to dm struct +20 (the field DryosD...
NSTUB(0xFE117474, dm_set_print_level)                      // twin of dm_set_store_level writing +21

/** Dialog API **/
NSTUB(0xFE3A3554, dialog_redraw)                           // ./Dialog/Dialog.c line 728 "pDialog != NULL", same code as 1100D (l...
NSTUB(0xFE2BA174, dialog_set_property_str)                 // same code: strlen + set dialog property type 10

/** Dialog handlers **/
NSTUB(0xFE52D5A0, ErrCardForLVApp_handler)                 // string signature of 1100D stub
NSTUB(0xFE3FA314, ErrForCamera_handler)                    // string signature of 1100D stub (0.77)
NSTUB(0xFE421C04, HideUnaviFeedBack_maybe)                 // same 8 instructions as 1100D (struct 0x3F550 +0x14 = 0x17, GUI post...
NSTUB(0xFE4267B0, LiveViewApp_handler)                     // "DlgLiveView.c FeedBackCancel(%#x)", "LOCAL_DIALOG_REFRESH_LV"
NSTUB(0xFE426E1C, LiveViewApp_handler_BL_JudgeBottomInfoDispTimerState) // the only BL to JudgeBottomInfoDispTimerState (0xFE423970) inside Li...
NSTUB(0xFE428458, LiveViewApp_handler_end)                 // "StartLiveViewApp(%#x)" (function after the handler)
NSTUB(0xFE4421A0, PlayMain_handler)                        // "DlgPlayMain.c CrossKeyEvent Event(0x%x)" etc.
NSTUB(0xFE568974, PlayMovieGuideApp_handler)               // string signature of 1100D stub
NSTUB(0xFE43595C, ShootOlcApp_handler)                     // string signature of 1100D stub (0.73)
// NSTUB(       ???, LiveViewLevelApp_handler)                /* present on 5D3.113 */
// NSTUB(       ???, LiveViewShutterApp_handler)              /* present on 6D.113, 650D.104, EOSM.202, 700D.113 */
// NSTUB(       ???, LiveViewWbApp_handler)                /* present on 7D.203, 5D3.113, 5D2.212 */
// NSTUB(       ???, LiveViewWifiApp_handler)                 /* present on 6D.113 */
// NSTUB(       ???, StartPlayProtectGuideApp)                /* present on 7D.203, 5D2.212 */
// NSTUB(       ???, StopPlayProtectGuideApp)                 /* present on 7D.203, 5D2.212 */

/** Display **/
NSTUB(0xFE3C0974, MirrorDisplay)                           // "-->Mirror start" debug msg + BL MirrorTFT (eventproc 0xFE110F1C)
NSTUB(0xFE3C09D4, NormalDisplay)                           // BL NormalTFT (eventproc 0xFE110F44)
NSTUB(0xFE3C09A4, ReverseDisplay)                          // BL ReverseTFT (eventproc 0xFE110EF4)

/** EDMAC **/
NSTUB(0xFE2771D8, AbortEDmac)                              // 0.67 match, EDMAC block keeps 1300D spacing (-0x276D0)
NSTUB(0xFE276EE8, ConnectReadEDmac)                        // string signature of 1100D stub
NSTUB(0xFE276E24, ConnectWriteEDmac)                       // string signature of 1100D stub
NSTUB(0xFE2772A0, RegisterEDmacAbortCBR)                   // string signature of 1100D stub
NSTUB(0xFE277278, RegisterEDmacCompleteCBR)                // 1.00 shape match
NSTUB(0xFE2773A4, RegisterEDmacPopCBR)                     // 1.00 shape match
NSTUB(0xFE276D6C, SetEDmac)                                // string signature of 1100D stub
NSTUB(0xFE277084, StartEDmac)                              // string signature of 1100D stub
NSTUB(0xFE2772E0, UnregisterEDmacAbortCBR)                 // string signature of 1100D stub
NSTUB(0xFE27728C, UnregisterEDmacCompleteCBR)              // 1.00 shape match
NSTUB(0xFE2773B8, UnregisterEDmacPopCBR)                   // 1.00 shape match

/** Electronic Level **/
NSTUB(0xFE2B2928, GUI_SetRollingPitchingLevelStatus)       // "GUI_SetRollingPitchingLevelStatus Status(%d)"

/** Engio **/
NSTUB(0xFE27816C, _EngDrvOut)                              // string signature of 1100D stub
NSTUB(0xFE278518, _engio_write)                            // string signature of 1100D stub
// NSTUB(       ???, shamem_read) //0xfe29f838
NSTUB(0xFE27805C, shamem_read)                             // 0.86 match, 1300D spacing to _EngDrvOut

/** Eventprocs (call by name) **/
NSTUB(0xFE117D60, call)                                    // 0.90 shape match (eventproc dispatcher)

/** Events **/
NSTUB(0xFE28EDA0, TryPostEvent)                            // TaskClass post (checks "TaskClass" signature); called before "GetFi...
NSTUB(0xFE28EE00, TryPostEvent_end)                        // start of the next function
NSTUB(0xFE28BFF0, TryPostStageEvent)                       // same code as TryPostEvent, checks the "StageClass" signature
NSTUB(0xFE28C050, TryPostStageEvent_end)                   // start of the next function

/** ExMem **/
NSTUB(0xFE119B98, AddMemoryChunk)                          // [medium] 0.68 match; ExMem block order as on 1300D
NSTUB(0xFE11AFFC, AllocateContinuousMemoryResource)        // 0.68 match; 1300D spacing (AllocateMemoryResource +0x4C)
NSTUB(0xFE11AFB0, AllocateMemoryResource)                  // 0.68 match; 1300D spacing
NSTUB(0xFE119334, CreateMemoryChunk)                       // 0.72 match
NSTUB(0xFE119940, CreateMemorySuite)                       // 0.77 match
NSTUB(0xFE1197B4, DeleteMemorySuite)                       // [medium] 0.39 match; GetMemoryAddressOfMemoryChunk +0x74 as on 1300D
NSTUB(0xFE11B114, FreeMemoryResource)                      // 0.76 match; 1300D spacing
NSTUB(0xFE119C68, GetFirstChunkFromSuite)                  // 0.81 match; aligned call in CreateSkeltonJob vs 1100D
NSTUB(0xFE119740, GetMemoryAddressOfMemoryChunk)           // 0.67 match; aligned call in CreateSkeltonJob vs 1100D
NSTUB(0xFE11A010, GetNextMemoryChunk)                      // [medium] 0.54 match
NSTUB(0xFE0D494C, SRM_AllocateMemoryResourceFor1stJob)     // call after _prop_request_change in CreateSkeltonJob, as on 1100D; "...
NSTUB(0xFE0D6F14, SRM_FreeMemoryResourceFor1stJob)         // 2 callers at the same places as on 1100D; "pPriorMessage" assert

/** GUI for old cameras **/
NSTUB(   0x364AC, gui_timer_struct)                        // RAM literal in GUI_Control_Post (1300D 0x36CAC - 0x800)
NSTUB(0xFE147D6C, gui_change_mode)                         // "GUI_ChangeMode_Post:%d, %d, %d"
NSTUB(0xFE14854C, gui_local_post)                          // same switch dispatcher as 1100D gui_local_post; 1300D spacing to gu...
NSTUB(0xFE148B74, gui_other_post)                          // string signature of 1100D stub
// NSTUB(       ???, gui_change_lcd_state_post)               /* present on 7D.203, 500D.111, 50D.109, 5D2.212 */

NSTUB(0xFE148F5C, gui_change_shoot_type_post)              // "GUI_ChangeShootType_Post"
NSTUB(0xFE1497E8, gui_init_event)                          // "-> handleGuiInit", "AllocateBmpVram"
// NSTUB(       ???, gui_post_10000085)                       /* present on 500D.111, 50D.109, 5D2.212 */
// NSTUB(       ???, gui_timer_something)                     /* present on 7D.203, 500D.111, 50D.109, 5D2.212 */

/** GUI timers **/
NSTUB(0xFE41C7C8, CancelDateTimer)                         // string signature of 1100D stub
NSTUB(0xFE11EB40, CancelTimer)                             // 0.81 match; called where 1100D ErrForCamera_handler calls CancelTimer
NSTUB(0xFE118480, SetHPTimerAfterNow)                      // 0.72 match
NSTUB(0xFE118574, SetHPTimerNextTick)                      // same code as 1100D (expiry = r0 + r1)
NSTUB(0xFE11EA70, SetTimerAfter)                           // 0.54 match; CancelTimer - 0xD0 as on 1300D

/** H264 Encoder **/
NSTUB(   0x38250, mvr_config)                              // struct written by mvrSetFullHDOptSize/mvrSetDefQScale
NSTUB(0xFE25BFA4, mvrFixQScale)                            // same code as 1100D (mvr_config +4 / +22)
NSTUB(0xFE25BA68, mvrSetDefQScale)                         // same code as 1100D (mvr_config +10)
NSTUB(0xFE25BAB0, mvrSetFullHDOptSize)                     // string signature of 1100D stub
NSTUB(0xFE25BC94, mvrSetGopOptSizeFULLHD)                  // string signature of 1100D stub

/** Interrupts **/
// NSTUB(       ???, isr_table_handler)                       /* present on 7D_MASTER.203 */
// NSTUB(       ???, isr_table_param)                         /* present on 7D_MASTER.203 */
NSTUB(   0x308AC, post_isr_hook)                           // called after the ISR in the IRQ handler (ROM 0xFE0C0738)
NSTUB(   0x308A8, pre_isr_hook)                            // called before the ISR in the IRQ handler (ROM 0xFE0C06DC)

/** LCD Sensor **/
NSTUB(0xFE121678, DispSensorStart)                         // "req DispSensorStart %d %d %d %d %d ( %d %d )"

/** Making the card bootable **/
NSTUB(   0x485C8, cf_device)                               // [medium] RAM literal of the CF driver match; sd_device - 0x24 as on 1300D
NSTUB(   0x485EC, sd_device)                               // SD_SetBusWidth-like function aligned with 1100D (0.85): 0x15560 -> ...
NSTUB(0xFE4C5EAC, fsuDecodePartitionTable)                 // 0.78 shape match

/** Memory allocation **/
NSTUB(0xFE11D574, GetMemoryInformation)                    // _AllocateMemory - 0x260 as on 1300D
NSTUB(0xFE11D520, GetSizeOfMaxRegion)                      // 0.75 match, _AllocateMemory - 0x2B4 as on 1300D
NSTUB(0xFE11D7D4, _AllocateMemory)                         // 0.76 match; called by _alloc_dma_memory
NSTUB(0xFE11DB70, _FreeMemory)                             // 0.75 match; called by _free_dma_memory
NSTUB(0xFE28EEC4, _alloc_dma_memory)                       // _AllocateMemory + cache line alignment, returns pointer | 0x40000000
NSTUB(0xFE2935A0, _free)                                   // 0.89 match, _malloc + 0x58 as on 1300D
NSTUB(0xFE28EF00, _free_dma_memory)                        // clears 0x40000000, tail-calls _FreeMemory
NSTUB(0xFE293548, _malloc)                                 // 1.00 shape match
// NSTUB(       ???, free_dma_memory)                         /* present on 7D_MASTER.203 */

/** Message queues **/
NSTUB(    0x7804, msg_queue_count)                         // SystemIF::KerQueue.c (line 561), 1300D - 0x64
NSTUB(    0x741C, msg_queue_create)                        // SystemIF::KerQueue.c (line 95), 1300D - 0x64
NSTUB(    0x77B0, msg_queue_post)                          // SystemIF::KerQueue.c (line 532), 0.86 match
NSTUB(    0x756C, msg_queue_receive)                       // SystemIF::KerQueue.c (lines 307-329), gui_main_task waits on it

/** PTP **/
NSTUB(0xFE32BB10, ptp_register_handler)                    // string signature of 1100D stub
// NSTUB(       ???, ptpPropSetUILock)  // not located yet (only used by the PTP CHDK option)

/** Properties **/
NSTUB(0xFE105314, _prop_cleanup)                           // 0.89 match
NSTUB(0xFE104D60, _prop_request_change)                    // 0.82 match; aligned call in CreateSkeltonJob vs 1100D
NSTUB(0xFE1051B8, prop_deliver)                            // 0.71 match
NSTUB(0xFE105240, prop_register_slave)                     // 0.75 match, prop_deliver + 0x88 as on 1300D
// NSTUB(       ???, prop_unregister_slave)                   /* present on 7D.203 */

/** Recursive locks **/
NSTUB(    0xB9B8, AcquireRecursiveLock)                    // SystemIF::KerRLock.c (lines 198-215), 1300D - 0x300
NSTUB(    0xB8F8, CreateRecursiveLock)                     // SystemIF::KerRLock.c (line 88), 1300D - 0x300
NSTUB(    0xBB28, ReleaseRecursiveLock)                    // SystemIF::KerRLock.c (lines 305-318), 1300D - 0x300

/** ResLock **/
NSTUB(0xFE27AB2C, CreateResLockEntry)                      // string signature of 1100D stub
NSTUB(0xFE27B06C, LockEngineResources)                     // string signature of 1100D stub
NSTUB(0xFE27B1A4, UnLockEngineResources)                   // string signature of 1100D stub

/** Semaphores **/
NSTUB(    0x3054, create_named_semaphore)                  // SystemIF::KerSem.c (line 94), 1300D - 0x74
NSTUB(    0x32E4, give_semaphore)                          // SystemIF::KerSem.c (line 354), 1300D - 0x74
NSTUB(    0x31FC, take_semaphore)                          // SystemIF::KerSem.c (lines 307-324), 0.80 match

/** Task info **/
NSTUB(   0x350EC, task_max)                                // loop bound of the task table init (stride 0x54) at RAM 0x3F9C

NSTUB(    0x24C4, _get_task_info_by_id)                    // copies task fields (entry +0xC, stack +0x1C, name +0x24, context +0...
NSTUB(   0x30978, current_task)                            // read by _get_task_info_by_id and task_trampoline; the QEMU model us...
NSTUB(     0x640, current_interrupt)                       // IRQ handler stores MEM(0xC0201004) here (copied block, ROM 0xFE0C0810)

/* Wi-Fi / sockets: not ported (the 1300D entries were never enabled) */

/** Task sleep **/
NSTUB(    0x370C, msleep)                                  // SystemIF::KerSys.c line 626; same code as 1100D msleep

/** Tasks **/
NSTUB(   0x35128, task_dispatch_hook)                      // called by the scheduler as hook(n, old, new) at RAM 0x1D20 / 0x1D80...
NSTUB(    0x3888, task_create)                             // called with every task name (tasks.csv); SystemIF::KerTask.c
NSTUB(    0x4168, task_trampoline)                         // calls current_task->entry(arg) and then the task exit path
// NSTUB(       ???, current_task_ctx)                        /* present on 7D.203 */
// NSTUB(       ???, next_task_ctx)                           /* present on 7D.203 */

/** Misc **/
NSTUB(   0x6B04C, lv_path_struct)                          // [medium] RAM literal of the matched 1100D function (0.83); not used by the m...
NSTUB(0xFE112DA8, LoadCalendarFromRTC)                     // string signature of 1100D stub
NSTUB(0xFE28A874, vsnprintf)                               // 0.79 match; called by DryosDebugMsg like on 1100D
// NSTUB(       ???, _print_serial)
// NSTUB(       ???, ChangeHDMIOutputSizeToFULLHD)
// NSTUB(       ???, ChangeHDMIOutputSizeToVGA)
NSTUB(0xFE10D398, EnableImagePhysicalScreenParameter)      // "EnableImagePhysicalScreenParameter" prefix
// NSTUB(       ???, FSUunMountDevice)                        /* present on 5D3.113 */
// NSTUB(       ???, PtpDps_remote_release_SW1_SW2_worker)    /* present on 7D.203, 5D2.212 */
// NSTUB(       ???, memcpy)                                  /* present on 7D_MASTER.203 */

NSTUB(    0x767C, TryReceiveMessageQueue)                  // [medium] SystemIF::KerQueue.c: non-blocking receive (returns 13 when empty)
