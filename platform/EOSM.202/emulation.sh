# Emulation profile of the EOS M (firmware 2.0.2) for ci/qemu/run_cam.sh (sourced; see docs/EMULATION.md,
# "Adding a camera"). Every value is VAR=${VAR:-default}, so an environment variable still overrides it.
# QEMU side of the same camera: QEMU-EOS-Optimized hw/eos/cams/EOSM/.

# Canon's power-save counter in RAM, sampled during the window: state block 0x3EE58, counter at 0x3EE5C
# (DisablePowerSave 0xFF137DDC / EnablePowerSave 0xFF137E48; canon-firmware-re notes, QEMU-EOS-Optimized
# docs/models/EOSM.md). No deep sleep is modelled by qemu-eos for this camera, so no display-off segment.
# The real camera agrees so far (capture kit, 2026-10): the counter is 2 in LiveView, the menus and playback
# (DisplayMgr while the display is on + CtrlSrv), reaches 0 only for ~12 ms on GUI-mode changes, and PowerMgr
# never left mode 1; how it enters selfRefresh (display off) has not been captured yet.
PM_COUNTER_ADDR=${PM_COUNTER_ADDR:-0x3EE5C}
DISPLAY_OFF_S=${DISPLAY_OFF_S:-0}

# Guest ready for the canon scenario: Canon's start-up task is done. The camera starts in LiveView with a muted
# grey screen (no sensor frames), so the display says nothing; this line (QEMU-EOS-Optimized docs/models/EOSM.md)
# does. Canon's power-save counter never reaches 0 here: no deep sleep to check.
CANON_READY=${CANON_READY:-startupInitializeComplete}

# Shooting scenarios: the pinned QEMU image has no photo capture emulation for this camera.
SHOOT_CANON_READY=${SHOOT_CANON_READY-}
SHOOT_SHUTTER_KEY=${SHOOT_SHUTTER_KEY-}
SHOOT_SCENARIOS=${SHOOT_SCENARIOS-}
SHOOT_NO_FILES=${SHOOT_NO_FILES-}

IVL_PHOTO=${IVL_PHOTO:-0}
PROBE_KEYS=${PROBE_KEYS-}
MENU_TOUR=${MENU_TOUR-}
PHOTO_MODULES=${PHOTO_MODULES-}

# No deep-sleep expectations that differ from compare.py (EXPECT_SLEEP).
EXPECT_SLEEP=${EXPECT_SLEEP-}
