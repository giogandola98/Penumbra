# Host-side characterization tests for the overlay pixel kernels.
#   make          build and run the tests (non-zero exit on any mismatch)
#   make bench    host timing, original vs new (indicative only)
#   make armcount ARM926 instruction counts, original vs new (see below)

CC      ?= gcc
CFLAGS  ?= -O2
CFLAGS  += -std=gnu99 -Wall -Wextra -Werror -I../../src -Ireference
SRC_DIR  = ../../src

TEST_SRCS = test_overlays.c reference/ref_overlays.c $(SRC_DIR)/overlay_kernels.c
TEST_HDRS = reference/ref_shim.h $(SRC_DIR)/overlay_kernels.h

.PHONY: all test bench clean
.DEFAULT_GOAL := test

all: test

test_overlays: $(TEST_SRCS) $(TEST_HDRS) Makefile
	$(CC) $(CFLAGS) -o $@ $(TEST_SRCS)

test: test_overlays
	./test_overlays

bench: test_overlays
	./test_overlays bench

clean:
	rm -f test_overlays arm/arm_driver.elf

# ARM926 instruction counts, original vs new, built like the EOSM firmware.
# Needs the pinned toolchain on PATH (. ~/.local/opt/ml-env.sh) and the
# 'unicorn' Python package; not part of the default target.
ARM_CC     ?= arm-none-eabi-gcc
ARM_CFLAGS  = -Os -march=armv5te -std=gnu99 -fomit-frame-pointer -fno-strict-aliasing \
              -ffast-math -nostdlib -ffreestanding -Wall -Wextra -Werror -Wno-unused-parameter \
              -I../../src -Ireference
PYTHON     ?= python3

arm/arm_driver.elf: arm/arm_driver.c $(TEST_HDRS) reference/ref_overlays.c $(SRC_DIR)/overlay_kernels.c Makefile
	$(ARM_CC) $(ARM_CFLAGS) -Wl,-Ttext=0x10000 -Wl,-e,setup -o $@ \
		arm/arm_driver.c reference/ref_overlays.c $(SRC_DIR)/overlay_kernels.c -lgcc

armcount: arm/arm_driver.elf
	$(PYTHON) arm/armcount.py $<

.PHONY: armcount
