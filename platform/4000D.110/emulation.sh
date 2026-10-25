# Emulation profile of the 4000D (firmware 1.1.0) for ci/qemu/run_cam.sh (sourced; see docs/EMULATION.md,
# "Adding a camera"). Every value is VAR=${VAR:-default}, so an environment variable still overrides it.
# QEMU side of the same camera: QEMU-EOS-Optimized hw/eos/cams/4000D/.

# Canon's power-save counter in RAM, sampled during the window. Same PowerSaver struct as the 1100D
# (docs/firmware/1100D-powersave.md): RAM 0x31648, +0 "PowerSaver" magic, +4 counter (docs/firmware/4000D-consts.md).
# Deep sleep (selfRefresh) is modelled by qemu-eos for this camera (docs/firmware/4000D-powersave.md).
PM_COUNTER_ADDR=${PM_COUNTER_ADDR:-0x3164C}

# Display-off segment after the window (INFO off, then back on) for the deep-sleep check;
# 0 skips it (docs/analysis/powersave-hold.md).
DISPLAY_OFF_S=${DISPLAY_OFF_S:-30}

# Guest ready for the canon scenario: none (the window is timed from launch). ML's marker is common (run_cam.sh).
CANON_READY=${CANON_READY-}

# Shooting scenarios (shoot.py): ready marker of the canon scenarios, key of a full shutter press, scenarios
# that run on this camera, and the ones whose picture files are not checked (releases only; none: since
# qemu-eos orders the JPEG output completion by its input, back-to-back HDR frames are developed to files
# like spaced shots).
SHOOT_CANON_READY=${SHOOT_CANON_READY:-startupInitializeComplete}
SHOOT_SHUTTER_KEY=${SHOOT_SHUTTER_KEY:-ctrl}
SHOOT_SCENARIOS=${SHOOT_SCENARIOS:-canon-shot ml-ivl ml-hdr ml-bulb}
SHOOT_NO_FILES=${SHOOT_NO_FILES-}

# Intervalometer auto-started by a config preset in the photo build (ml-photo-ivl): no deep sleep while the
# hold is taken.
IVL_PHOTO=${IVL_PHOTO:-1}

# Port bring-up: log the GUI events behind each emulated key in the canon scenario (scenario.py --probe-keys),
# which is how gui.h is checked (docs/firmware/4000D-consts.md).
PROBE_KEYS=${PROBE_KEYS-m,m,delete,i,i,q,q,p,p,spc,a,up,down,left,right,z,x}
# Port bring-up: walk the ML menu tabs in the ML scenarios (scenario.py --menu-tour; right = next tab, wraps).
MENU_TOUR=${MENU_TOUR-right,right,right,right,right,right,right}
# Modules switched on (ML/SETTINGS/<name>.en) in the photo scenarios, so they are loaded at boot. ML ships
# modules disabled; sd_clock (SD overclock) stays off on purpose.
PHOTO_MODULES=${PHOTO_MODULES-file_man img_name lua}

# Deep-sleep expectations that differ from the generic ones in compare.py (EXPECT_SLEEP), "scenario=0|1":
# the photo profile forwards the counted holds (CONFIG_POWERSAVE_HOLD), so ml-photo sleeps like ml-hold.
EXPECT_SLEEP=${EXPECT_SLEEP-ml-photo=1}
