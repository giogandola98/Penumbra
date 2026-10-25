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
else ifeq ($(ML_PROFILE),capture)
    # Read-only data capture kit for emulation work (docs/CAPTURE-KIT.md): menu + captures + card writes only.
    # features-capture.h replaces the whole feature set; no modules, no TCC, no installer FIR in the zip;
    # Canon stubs that write settings/flash/registers are not even assembled (stubs.S, CONFIG_PROFILE_CAPTURE),
    # and tools/check_capture_kit.py fails the build if the linked binary references a forbidden routine.
    CFLAGS_PLATFORM += -DCONFIG_PROFILE_CAPTURE
    override AFLAGS += -DCONFIG_PROFILE_CAPTURE
    override CONFIG_MODULES = n
    override CONFIG_TCC = n
    override CONFIG_CONSOLE = y
    ML_NO_INSTALLER_FIR = y
    POWERSAVE_HOLD ?= y
    ML_BOOTFLAGS_OBJ = n
    ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/capture-kit.o
    CAPTURE_CHECK = y
else
    $(error Unknown ML_PROFILE '$(ML_PROFILE)' (expected photo, full or capture))
endif

include ../Makefile
.DEFAULT_GOAL := $(BUILD_DIR)/cam_complete

ifeq ($(CAPTURE_CHECK),y)
# Build-time safety check of the capture kit: fails (and the zip is not marked complete) if the linked
# binary can reach a forbidden Canon routine or the zip ships anything but autoexec.bin and ML data.
$(BUILD_DIR)/cam_complete: $(BUILD_DIR)/capture-check.txt
$(BUILD_DIR)/capture-check.txt: $(BUILD_DIR)/magiclantern.zip $(BUILD_DIR)/magiclantern capture-forbidden.txt \
                                $(TOP_DIR)/tools/check_capture_kit.py
	$(PYTHON) -I $(TOP_DIR)/tools/check_capture_kit.py --elf $(BUILD_DIR)/magiclantern \
	    --autoexec $(BUILD_DIR)/autoexec.bin --zip $(BUILD_DIR)/magiclantern.zip \
	    --forbidden capture-forbidden.txt > $@.tmp || { cat $@.tmp; rm -f $@.tmp; exit 1; }
	@cat $@.tmp; mv $@.tmp $@
endif
