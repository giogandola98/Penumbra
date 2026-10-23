/*
 *  4000D 1.1.0 consts
 *
 *  Values marked "4000D:" were located in this firmware (how: comment, details in
 *  docs/firmware/4000D-stubs.md). Values marked "1300D value, not verified" belong to
 *  LiveView / movie / raw code paths, which this port does not enable yet; they only keep
 *  shared code compiling and must be checked before the matching feature is turned on.
 */

#define CANON_SHUTTER_RATING 100000

// 1300D values, not verified: the register is in the 4000D GPIO table (ROM 0xFE8284C4) and both
// values are used by the firmware, but the LED itself has not been seen blinking yet.
#define CARD_LED_ADDRESS 0xC022D1E0
#define LEDON 0x93D800
#define LEDOFF 0x83DC00

// boot-d45-ch.c, cache hack boot process
#define HIJACK_CACHE_HACK
#define HIJACK_CACHE_HACK_BSS_END_ADDR 0xFE0C1B74      // 4000D: MOV R1, #0xD00000 (end of Canon's AllocateMemory pool)
#define HIJACK_CACHE_HACK_BSS_END_INSTR 0xE3A01732     // MOV R1, #0xC80000 (= RESTARTSTART)
#define HIJACK_CACHE_HACK_INITTASK_ADDR 0xFE0C3A9C     // 4000D: literal holding init_task, passed by cstart to create_init_task

#define DRYOS_ASSERT_HANDLER 0x3508C // 4000D: assert routine (RAM 0x3C48) calls the handler stored here

// Movie recorder: 1300D values, not verified (movie mode is off)
#define MVR_992_STRUCT (*(void **)(0x315dc + 0x4))
#define MVR_FRAME_NUMBER (*(int *)(332 + MVR_992_STRUCT))
#define MVR_BYTES_WRITTEN MEM((296 + MVR_992_STRUCT))
#define MVR_BUFFER_USAGE 70 // wrong, but needs to be non-zero to avoid a compiler warning

#define MOV_RES_AND_FPS_COMBINATIONS 9
#define MOV_OPT_NUM_PARAMS 2
#define MOV_GOP_OPT_NUM_PARAMS 5
#define MOV_OPT_STEP 5
#define MOV_GOP_OPT_STEP 5

#define AUDIO_MONITORING_HEADPHONES_CONNECTED 0

// LiveView buffers: 1300D values, not verified (LiveView is off)
#define YUV422_LV_BUFFER_1 0x40d07800
#define YUV422_LV_BUFFER_2 0x4c233800
#define YUV422_LV_BUFFER_3 0x4f11d800

#define DEFAULT_RAW_BUFFER MEM(MEM(0x37930 + 0x30)) // 1300D value, not verified (raw is off)

#define REG_EDMAC_WRITE_LV_ADDR 0xc0f04308 // SDRAM address of LV buffer (aka VRAM)
#define REG_EDMAC_WRITE_HD_ADDR 0xc0f04208 // SDRAM address of HD buffer (aka YUV)

// 4000D: display manager struct 0x31010 (UpdateReverseTFT uses +0x7C, the display state object is +0xA0)
#define YUV422_LV_BUFFER_DISPLAY_ADDR (*(uint32_t *)(0x31010 + 0xB0))
#define YUV422_HD_BUFFER_DMA_ADDR (shamem_read(REG_EDMAC_WRITE_HD_ADDR))

// 1300D values, not verified (movie is off)
#define YUV422_HD_BUFFER_1 0x44000080
#define YUV422_HD_BUFFER_2 0x46000080
#define YUV422_HD_BUFFER_3 0x48000080
#define YUV422_HD_BUFFER_4 0x4e000080
#define YUV422_HD_BUFFER_5 0x50000080

#define FOCUS_CONFIRMATION 0 // not located yet (LiveView focus info is off)

// 4000D: main controller ([MC]) struct 0x351A4; DisplaySensorStop writes +0x18.
// +0x1C is the half-shutter flag by analogy with the 1300D (0x359A0 + 0x1C), not verified yet.
#define HALFSHUTTER_PRESSED (*(int *)(0x351A4 + 0x1C))
#define DISPLAY_SENSOR_POWERED (*(int *)(0x351A4 + 0x18))

// for gui_main_task
#define GMT_NFUNCS 7
#define GMT_FUNCTABLE 0xFE7F6134 // 4000D: table copied by gui_main_task (7 entries, 28 bytes)

// LiveView bottom bar / ISO adjustment: not located (LiveView is off); never active
#define LV_BOTTOM_BAR_DISPLAYED 0
#define ISO_ADJUSTMENT_ACTIVE 0
#define SHOOTING_MODE (*(int *)0x3592C) // 1300D value, not verified (unused by this build)
#define UNAVI_FEEDBACK_TIMER_ACTIVE (MEM(0x3F550 + 0x14) != 0x17) // 4000D: struct written by HideUnaviFeedBack

#define AE_STATE 0 // not located (LiveView exposure is off)
#define AE_VALUE 0

#define CURRENT_GUI_MODE (*(int *)(0x35D08 + 0x48)) // 4000D: SetGUIRequestMode stores the request at +0x4C, mode at +0x48 (1100D: 0x3918)

#define COLOR_FG_NONLV 80
#define GUIMODE_WB 5
#define GUIMODE_FOCUS_MODE 9
#define GUIMODE_DRIVE_MODE 8
#define GUIMODE_PICTURE_STYLE 4
#define GUIMODE_PLAY 1
#define GUIMODE_MENU 2
#define GUIMODE_Q_UNAVI 0x1F
#define GUIMODE_FLASH_AE 0x22
#define GUIMODE_PICQ 6
#define GUIMODE_MOVIE_ENSURE_A_LENS_IS_ATTACHED (CURRENT_GUI_MODE == 0x1e)
#define GUIMODE_MOVIE_PRESS_LV_TO_RESUME (CURRENT_GUI_MODE == 0x1f)

#define PLAY_MODE (gui_state == GUISTATE_PLAYMENU && CURRENT_GUI_MODE == GUIMODE_PLAY)
#define MENU_MODE (gui_state == GUISTATE_PLAYMENU && CURRENT_GUI_MODE == GUIMODE_MENU)

// Photo-mode info screen positions (720x480 BMP overlay, same layout as the 1300D)
#define MENU_DISP_ISO_POS_X 527
#define MENU_DISP_ISO_POS_Y 45
#define MAX_ISO_POS_X 590
#define MAX_ISO_POS_Y 28
#define HDR_STATUS_POS_X 562
#define HDR_STATUS_POS_Y 100
#define HTP_STATUS_POS_X 500
#define HTP_STATUS_POS_Y 233
#define MLU_STATUS_POS_X 316
#define MLU_STATUS_POS_Y 310
#define WBS_GM_POS_X 365
#define WBS_GM_POS_Y 230
#define WBS_POS_X 365
#define WBS_POS_Y 260
#define AUDIO_REM_SHOT_POS_X 200
#define AUDIO_REM_SHOT_POS_Y 386
#define DISPLAY_CLOCK_POS_X 440
#define DISPLAY_CLOCK_POS_Y 400
#define DISPLAY_DATE_POS_X 425
#define DISPLAY_DATE_POS_Y 430
#define DISPLAY_BATTERY_POS_X 350
#define DISPLAY_BATTERY_POS_Y 400
#define WB_K_ICON_POS_X 192
#define WB_K_ICON_POS_Y 226
#define WB_KELVIN_POS_X 192
#define WB_KELVIN_POS_Y 260
#define DISPLAY_GB_POS_X 305
#define DISPLAY_GB_POS_Y 410
#define DISPLAY_TRAP_FOCUS_POS_X 65
#define DISPLAY_TRAP_FOCUS_POS_Y 360
#define DISPLAY_TRAP_FOCUS_MSG "TRAP FOCUS"
#define DISPLAY_TRAP_FOCUS_MSG_BLANK "          "

// LiveView is off, so the ML menu always uses the photo-mode GUI mode
#define GUIMODE_ML_MENU 2

#define NUM_PICSTYLES 10

#define FLASH_MAX_EV 3
#define FLASH_MIN_EV -5
#define FASTEST_SHUTTER_SPEED_RAW 152
#define MAX_AE_EV 5

// 4000D: CreateDialogBox results of DlgMnCardFormatBegin (0xFE50B230) and of the
// DlgMnCardFormatExecute function aligned with the 1100D one (unused: CONFIG_RESTORE_AFTER_FORMAT is off)
#define DIALOG_MnCardFormatBegin (0x45944 + 4)
#define DIALOG_MnCardFormatExecute (0x492A4 + 4)
#define FORMAT_BTN_NAME "[Q]"
#define FORMAT_BTN BGMT_Q
#define FORMAT_STR_LOC 11

#define BULB_MIN_EXPOSURE 1000

// 4000D: Canon font ("FNT" header at 0xFEDDEB30): char codes at +0x24, then two tables of 0x2C04 bytes
#define BFNT_CHAR_CODES    0xFEDDEB54
#define BFNT_BITMAP_OFFSET 0xFEDE1758
#define BFNT_BITMAP_DATA   0xFEDE435C

// from CFn
#define AF_BTN_HALFSHUTTER 0
#define AF_BTN_STAR 1

#define IMGPLAY_ZOOM_LEVEL_ADDR (0x3A810 + 12) // 4000D: counter decremented by GuiImageZoomDown
#define IMGPLAY_ZOOM_LEVEL_MAX 14
// 1300D values, not verified (playback zoom features are off)
#define IMGPLAY_ZOOM_POS_X MEM(0x6FCC4)
#define IMGPLAY_ZOOM_POS_Y MEM(0x6FCC4 + 0x4)
#define IMGPLAY_ZOOM_POS_X_CENTER 0x144
#define IMGPLAY_ZOOM_POS_Y_CENTER 0xd8
#define IMGPLAY_ZOOM_POS_DELTA_X (0x144 - 0x93)
#define IMGPLAY_ZOOM_POS_DELTA_Y (0xd8 - 0x7d)

#define BULB_EXPOSURE_CORRECTION 100 // min value for which bulb exif is OK [not tested]

#define WINSYS_BMP_DIRTY_BIT_NEG MEM(0x3D2F0 + 0x2C) // 4000D: checkdraftvram aligned with the 1100D one (0xB198 -> 0x3D2F0)

// manual exposure overrides: 1300D values, not verified (LiveView is off)
#define LVAE_STRUCT 0x3B7A4
#define CONTROL_BV (*(uint16_t *)(LVAE_STRUCT + 0x1c))
#define CONTROL_BV_TV (*(uint16_t *)(LVAE_STRUCT + 0x1e))
#define CONTROL_BV_AV (*(uint16_t *)(LVAE_STRUCT + 0x20))
#define CONTROL_BV_ISO (*(uint16_t *)(LVAE_STRUCT + 0x22))
#define CONTROL_BV_ZERO (*(uint16_t *)(LVAE_STRUCT + 0x24))
#define LVAE_ISO_SPEED (*(uint8_t *)(LVAE_STRUCT))
#define LVAE_ISO_MIN (*(uint8_t *)(LVAE_STRUCT + 0x2a))
#define LVAE_ISO_HIS (*(uint8_t *)(LVAE_STRUCT + 0x2c))
#define LVAE_DISP_GAIN (*(uint16_t *)(LVAE_STRUCT + 0x26))
#define LVAE_MOV_M_CTRL (*(uint8_t *)(LVAE_STRUCT + 0x78))

#define DISPLAY_ORIENTATION MEM(0x31010 + 0x7C) // 4000D: read-only; UpdateReverseTFT

#define MIN_MSLEEP 20

#define INFO_BTN_NAME "DISP"
#define Q_BTN_NAME "[Q]"

#define DISPLAY_STATEOBJ (*(struct state_object **)(0x31010 + 0xA0)) // 4000D: state object created by the display manager init
#define DISPLAY_IS_ON (DISPLAY_STATEOBJ->current_state != 0)

// 1300D values, not verified (LiveView / FPS override are off)
#define VIDEO_PARAMETERS_SRC_3 0x6A95C
#define FRAME_SHUTTER_TIMER (*(uint16_t *)(VIDEO_PARAMETERS_SRC_3 + 0xC))
#define FRAME_ISO (*(uint8_t *)(VIDEO_PARAMETERS_SRC_3 + 0x8))
#define FRAME_APERTURE (*(uint8_t *)(VIDEO_PARAMETERS_SRC_3 + 0x9))
#define FRAME_SHUTTER (*(uint8_t *)(VIDEO_PARAMETERS_SRC_3 + 0xa))
#define FRAME_BV ((int)FRAME_SHUTTER + (int)FRAME_APERTURE - (int)FRAME_ISO)

#define FRAME_SHUTTER_BLANKING_ZOOM (*(uint16_t *)0x40481B20)
#define FRAME_SHUTTER_BLANKING_NOZOOM (*(uint16_t *)0x40481B24)
#define FRAME_SHUTTER_BLANKING_READ (lv_dispsize > 1 ? FRAME_SHUTTER_BLANKING_NOZOOM : FRAME_SHUTTER_BLANKING_ZOOM)
#define FRAME_SHUTTER_BLANKING_WRITE (lv_dispsize > 1 ? &FRAME_SHUTTER_BLANKING_ZOOM : &FRAME_SHUTTER_BLANKING_NOZOOM)

// 4000D: the struct _malloc locks ("Malloc Information"); 1100D uses its _malloc literal the same way
#define MALLOC_STRUCT_ADDR 0x64B94
#define MALLOC_FREE_MEMORY (MEM(MALLOC_STRUCT_ADDR + 8) - MEM(MALLOC_STRUCT_ADDR + 0x1C)) // "Total Size" - "Allocated Size"
#define SRM_BUFFER_SIZE 0x1f68000 // 1300D value (same sensor size), to print from srm_malloc_cbr
#define SRM_MAX_BUF_COUNT_VIDEO_MODE 3

// for bulb ramping calibration: delay between two exposure readings
#define BRAMP_CALIBRATION_DELAY 1000

//~ max volume supported for beeps
#define ASIF_MAX_VOL 5

#define JUDGE_BOTTOM_INFO_DISP_TIMER_STATE (0x3F550 + 0x5C) // 4000D: read by JudgeBottomInfoDispTimerState

// temperature conversion from raw-temperature to celsius (1300D formula, not verified)
#define EFIC_CELSIUS ((int)efic_temp * 55 / 100 - 68)
