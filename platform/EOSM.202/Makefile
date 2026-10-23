MODEL = EOSM
FW_VERSION = 202

TOP_DIR = ../..

AUTOEXEC_BASE = 0x800000
FIR_BASE = 0x800120
ML_BOOT_OBJ = boot-d45.o

#CANON_FIR = EOSM1202.FIR

MAIN_FIRMWARE_ADDR = 0xFF0C0000
PLATFORM_ARCH = armv5te

# DryOSmemory map
# RESTARTSTART is selected to be just above the end of the bss
#
RESTARTSTART = 0x9E1E0

ML_AF_PATTERNS_OBJ = n

# Build profile, see docs/BUILD-PROFILES.md
#   make                    -> photo profile (default for this fork)
#   make ML_PROFILE=full    -> upstream-equivalent feature set
ML_PROFILE ?= photo

ifeq ($(ML_PROFILE),photo)
    CFLAGS_PLATFORM += -DCONFIG_PROFILE_PHOTO
    MODULES_INCLUDED = modules-photo.included
    # CPU powersave held only while needed (docs/analysis/powersave-hold.md); POWERSAVE_HOLD=n to opt out
    POWERSAVE_HOLD ?= y
    # video-only code: bitrate hacks, audio codec, FPS override, movie tweaks, crop mode
    ML_SRC_OBJS_EXCLUDE =
else ifeq ($(ML_PROFILE),full)
    CFLAGS_PLATFORM += -DCONFIG_PROFILE_FULL
else
    $(error Unknown ML_PROFILE '$(ML_PROFILE)' (expected photo or full))
endif

include ../Makefile
.DEFAULT_GOAL := $(BUILD_DIR)/cam_complete
