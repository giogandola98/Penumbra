/* 4000D 1.1.0: minimal bring-up feature set (no LiveView, no movie, no raw, no fps).
 * Photo features are enabled one by one once verified in emulation (docs/PORT-4000D.md, step 5). */

#define FEATURE_SHOW_SHUTTER_COUNT
#define FEATURE_SHOW_FREE_MEMORY
#define FEATURE_SCREENSHOT

#define CONFIG_TSKMON
#define FEATURE_SHOW_TASKS
#define FEATURE_SHOW_CPU_USAGE
#define FEATURE_SHOW_GUI_EVENTS

#define CONFIG_CRASH_LOG

#undef CONFIG_ADDITIONAL_VERSION
#undef CONFIG_AUTOBACKUP_ROM
#undef CONFIG_PROP_REQUEST_CHANGE

/* Photo profile (default build, docs/BUILD-PROFILES.md): photo features on top of the bring-up set */
#ifdef CONFIG_PROFILE_PHOTO
#include "features-photo.h"
#endif
