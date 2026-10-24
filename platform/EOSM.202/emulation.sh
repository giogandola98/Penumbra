# Emulation profile of the EOS M (firmware 2.0.2) for ci/qemu/run_cam.sh (sourced; see docs/EMULATION.md,
# "Adding a camera"). Every value is VAR=${VAR:-default}, so an environment variable still overrides it.
# QEMU side of the same camera: QEMU-EOS-Optimized hw/eos/cams/EOSM/.

# No Canon power-save counter address known and no deep sleep modelled by qemu-eos: no counter sampling,
# no display-off segment.
PM_COUNTER_ADDR=${PM_COUNTER_ADDR-}
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
