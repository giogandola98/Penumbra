MODEL = 4000D
FW_VERSION = 110

TOP_DIR = ../..

AUTOEXEC_BASE = 0x800000
FIR_BASE = 0x800120

ML_MODULES_SYM_NAME = t100_$(FW_VERSION).sym

# Firmware entry; ROMBASEADDR 0xFF0C0000 is the same flash through another alias,
# but every firmware pointer uses the 0xFE000000 one.
MAIN_FIRMWARE_ADDR = 0xFE0C0000
PLATFORM_ARCH = armv5te

# DryOS memory map: BSS ends at 0x829D4 (cstart clear loop). Canon's AllocateMemory pool is
# 0x2D0000..0xD00000 (MOV R1,#0xD00000 at 0xFE0C1B74); the cache hack lowers its end to
# RESTARTSTART, giving ML 0xC80000..0xD00000, as on the 1300D.
RESTARTSTART = 0xC80000
ML_BOOT_OBJ = boot-d45-ch.o

# "Canon EOS Rebel T100" model record at 0xFE040044 carries this ID
FIRMWARE_ID_4000D = 0x80000422
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/function_overrides.o

include ../Makefile
.DEFAULT_GOAL := $(BUILD_DIR)/cam_complete
