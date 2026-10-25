/* EOS M 2.0.2 capture kit (ML_PROFILE=capture, docs/CAPTURE-KIT.md): internals that would let ML change
 * Canon state (properties, LiveView, image-engine registers, Canon's format dialog) are switched off.
 *
 * Included at the end of internals.h, not from features.h: vram.h, arm-mcr.h and rom_values.h include
 * internals.h again, so an #undef placed in features.h would be silently undone in those files.
 * src/capture-kit.c #errors if any of these is defined again; tools/check_capture_kit.py checks the binary. */

/* Canon property writes: prop_request_change() becomes a no-op that logs (src/property.c) */
#undef CONFIG_PROP_REQUEST_CHANGE
#undef CONFIG_PROP_WRITE_WHITELIST

/* ML config file: kept, the ML core does not build without it */
/* (MAGIC.CFG holds ML menu state only; no Canon setting can be reached from it in this feature set) */

/* No automatic 32 MiB ROM backup at first boot (the maintainer has the ROMs; less card traffic) */
#undef CONFIG_AUTOBACKUP_ROM

/* No hijack of Canon's format dialog (it rewrites ML onto the card after a format) */
#undef CONFIG_RESTORE_AFTER_FORMAT

/* No LiveView / raw / FPS / exposure overrides (these write image-engine registers) */
#undef CONFIG_RAW_LIVEVIEW
#undef CONFIG_RAW_PHOTO
#undef CONFIG_EDMAC_RAW_SLURP
#undef CONFIG_FRAME_ISO_OVERRIDE
#undef CONFIG_FRAME_ISO_OVERRIDE_ANALOG_ONLY
#undef CONFIG_FRAME_SHUTTER_OVERRIDE
#undef CONFIG_FPS_AGGRESSIVE_UPDATE
#undef CONFIG_FPS_UPDATE_FROM_EVF_STATE
#undef CONFIG_EXPSIM
#undef CONFIG_WB_WORKAROUND
#undef CONFIG_DISPLAY_FILTERS
#undef CONFIG_CAN_REDIRECT_DISPLAY_BUFFER
#undef CONFIG_CAN_REDIRECT_DISPLAY_BUFFER_EASILY
#undef CONFIG_LVAPP_HACK_DEBUGMSG
#undef CONFIG_MOVIE_AE_WARNING

/* Developer pokes */
#undef CONFIG_DIGIC_POKE
#undef CONFIG_ADDITIONAL_VERSION
