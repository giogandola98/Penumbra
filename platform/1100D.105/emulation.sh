# Emulation profile of the 1100D (firmware 1.0.5) for ci/qemu/run_cam.sh (sourced; see docs/EMULATION.md,
# "Adding a camera"). Every value is VAR=${VAR:-default}, so an environment variable still overrides it.
# QEMU side of the same camera: QEMU-EOS-Optimized hw/eos/cams/1100D/.

# Canon's power-save counter in RAM (docs/firmware/1100D-powersave.md), sampled during the window.
# Deep sleep (selfRefresh) is modelled by qemu-eos for this camera (docs/EMULATION.md).
PM_COUNTER_ADDR=${PM_COUNTER_ADDR:-0x2B64}

# Display-off segment after the window (INFO/DISP off, then back on) for the deep-sleep check;
# 0 skips it (docs/analysis/powersave-hold.md).
DISPLAY_OFF_S=${DISPLAY_OFF_S:-30}

# Guest ready (scenario.py --ready-marker, DryosDebugMsg log) for the canon scenario: the display is on at
# the end of boot. ML's marker is common to all platforms (run_cam.sh).
CANON_READY=${CANON_READY:-TurnOnDisplay finish}

# Shooting scenarios (shoot.py): ready marker of the canon scenarios, key of a full shutter press, scenarios
# that run on this camera, and the ones whose picture files are not checked (releases only).
SHOOT_CANON_READY=${SHOOT_CANON_READY:-TurnOnDisplay finish}
SHOOT_SHUTTER_KEY=${SHOOT_SHUTTER_KEY:-ctrl}
SHOOT_SCENARIOS=${SHOOT_SCENARIOS:-canon-shot ml-ivl ml-hdr ml-bulb}
SHOOT_NO_FILES=${SHOOT_NO_FILES-}

# Intervalometer auto-started by a config preset in the photo build (ml-photo-ivl): not on this camera,
# whose active hold is covered by ml-hold-ivl (the display-off segment, DISPLAY_OFF_S > 0).
IVL_PHOTO=${IVL_PHOTO:-0}

# Port bring-up only (4000D): key probe in the canon scenario, ML menu tour, modules switched on.
PROBE_KEYS=${PROBE_KEYS-}
MENU_TOUR=${MENU_TOUR-}
PHOTO_MODULES=${PHOTO_MODULES-}

# Deep-sleep expectations that differ from the generic ones in compare.py (EXPECT_SLEEP), "scenario=0|1".
EXPECT_SLEEP=${EXPECT_SLEEP-}
