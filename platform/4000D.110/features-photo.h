/* Photo profile for the 4000D: photo features that do not need LiveView, enabled one at a time,
 * each one built with FATAL_WARNINGS=y and booted in emulation (docs/PORT-4000D.md, step 5).
 * Included at the end of features.h when CONFIG_PROFILE_PHOTO is set. */

/* Shoot menu */
#define FEATURE_INTERVALOMETER

/* Canon property writes, restricted to platform/4000D.110/property_whitelist.h */
#define CONFIG_PROP_REQUEST_CHANGE
#define CONFIG_PROP_WRITE_WHITELIST

#define FEATURE_HDR_BRACKETING
#define FEATURE_BULB_TIMER
