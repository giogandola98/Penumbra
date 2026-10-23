/**
 * Camera internals for 4000D 1.1.0
 *
 * Minimal bring-up set, derived from 1300D.110 (same DIGIC 4+ / DryOS r58 generation).
 * LiveView, movie and raw related options stay off until their constants are verified
 * on this firmware (docs/PORT-4000D.md).
 */

/** This camera has a DIGIC IV chip (DIGIC 4+) */
#define CONFIG_DIGIC_IV

/** Task switch hook is called as hook(n, old_task, new_task): RAM 0x1D20 / 0x1D80 */
#define CONFIG_NEW_DRYOS_TASK_HOOKS

/** State object hooks run inside Canon tasks; none are verified yet on this camera */
//~ #define CONFIG_STATE_OBJECT_HOOKS

/** This camera has an APS-C sensor */
//~ #define CONFIG_FULLFRAME

/** LiveView and movie mode exist on the camera, but are not supported by this port yet **/
//~ #define CONFIG_LIVEVIEW
//~ #define CONFIG_MOVIE

/** 2.7" 4:3 screen, 720x480 BMP overlay (same as 1300D) **/
#define CONFIG_4_3_SCREEN

/** No LCD sensor, no MLU, no electronic level, no flip-out display */
//~ #define CONFIG_LCD_SENSOR
//~ #define CONFIG_MLU
//~ #define CONFIG_ELECTRONIC_LEVEL
//~ #define CONFIG_VARIANGLE_DISPLAY

/** There is a Q menu in Play mode **/
#define CONFIG_Q_MENU_PLAYBACK

/** Bulb is done in M mode by going beyond 30s **/
#define CONFIG_BULB

/** No beeps via ASIF DMA (not verified on this camera) **/
#undef CONFIG_BEEP

/** We can restore ML files after formatting the card in the camera **/
//~ #define CONFIG_RESTORE_AFTER_FORMAT

/** FIO_RenameFile works (stub found, not exercised yet) **/
#define CONFIG_FIO_RENAMEFILE_WORKS

/** No ML menu key on the 1300D/4000D layout: the exposure compensation (Av) button is used,
 *  as on the 1300D; the Trash key also opens it (gui.h) **/
#define CONFIG_MENU_WITH_AV

#define CONFIG_MALLOC_STRUCT_V2

/** DryOS task / task_attr layout as read by _get_task_info_by_id (RAM 0x24C4): task struct
 *  0x54 bytes, name +0x24, id +0x40, state +0x49, context +0x50; attr name +0x34 */
#define CONFIG_TASK_STRUCT_V2
#define CONFIG_TASK_ATTR_STRUCT_V4
