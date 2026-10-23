/* Photo-only profile for the EOS M: undefine video / audio / dev-only features.
 * Included at the end of features.h when CONFIG_PROFILE_PHOTO is set.
 * Anything not listed here keeps the upstream default. */

/* Audio */
#undef FEATURE_AUDIO_METERS
#undef FEATURE_AUDIO_REMOTE_SHOT    /* needs the audio path */
#undef FEATURE_BEEP

/* Movie menu */
#undef FEATURE_NITRATE              /* bitrate.c */
#undef FEATURE_REC_INDICATOR
#undef FEATURE_MOVIE_LOGGING
#undef FEATURE_MOVIE_RESTART
#undef FEATURE_REC_NOTIFY
#undef FEATURE_MOVIE_REC_KEY
#undef FEATURE_GRADUAL_EXPOSURE
#undef FEATURE_FPS_OVERRIDE
#undef FEATURE_FPS_RAMPING
#undef FEATURE_HDR_VIDEO
#undef FEATURE_VIGNETTING_CORRECTION
#undef FEATURE_SHUTTER_FINE_TUNING
#undef FEATURE_CROP_MODE_HACK       /* video crop-mode registers; crop_rec module is dropped too */
#undef FEATURE_VIDEO_HACKS

/* Debug / dev-only */
#undef FEATURE_SHOW_EDMAC_INFO
