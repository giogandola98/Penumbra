/* EOS M 2.0.2 capture kit (ML_PROFILE=capture, docs/CAPTURE-KIT.md).
 *
 * Read-only data collection for the emulator: ML menu, the capture actions of src/capture-kit.c and
 * file writes to the card. Included from features.h *instead of* all_features.h, so none of the FEATURE_*
 * of the normal builds exists here. The CONFIG_* switched off are in internals-capture.h. */

/* Kept: crash log (written to the card on an ML assert), task monitor (read-only CPU accounting). */
#define CONFIG_CRASH_LOG
#define CONFIG_TSKMON
