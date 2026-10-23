# Host-side tests: overlay pixel kernels (characterization), powersave hold bookkeeping and the
# power-save hold around ML's card I/O (src/fio-ml.c built against fio_shim/).
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

test_powersave_hold: test_powersave_hold.c $(SRC_DIR)/powersave_hold.h Makefile
	$(CC) $(CFLAGS) -o $@ test_powersave_hold.c

# fio-ml.c is compiled from a copy so that its #include "..." resolve to fio_shim/, not src/;
# flags as for the firmware (-Wno-format, -Wno-unused-*); 32-bit address casts are fine there
build/fio-ml.c: $(SRC_DIR)/fio-ml.c
	mkdir -p build
	cp $< $@

FIO_SHIM = $(wildcard fio_shim/*.h)
test_fio_hold: test_fio_hold.c build/fio-ml.c $(FIO_SHIM) $(SRC_DIR)/fio-ml.h Makefile
	$(CC) -Ifio_shim $(CFLAGS) -Wno-format -Wno-unused-function -Wno-unused-parameter -Wno-int-to-pointer-cast -o $@ test_fio_hold.c build/fio-ml.c

test: test_overlays test_powersave_hold test_fio_hold
	./test_overlays
	./test_powersave_hold
	./test_fio_hold

bench: test_overlays
	./test_overlays bench

clean:
	rm -f test_overlays test_powersave_hold test_fio_hold arm/arm_driver.elf
	rm -rf build

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
