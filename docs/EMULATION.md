# Emulation (qemu-eos)

Phase 3 of `docs/PLAN.md`: boot the Canon firmware (and ML) headless in QEMU and measure how often the
CPU leaves idle. Everything lives in `ci/qemu/`; CI runs it in the `emulation` job of
`.forgejo/workflows/ci.yml`.

## Pieces

QEMU itself lives in the separate repo **QEMU-EOS-Optimized** (fork
of qemu-eos `qemu-eos-v4.2.1`). Its CI builds qemu-eos from that repo's source, runs the plugin self-test,
boots the 1100D to the Canon GUI with scripted menu scenarios, and publishes the image to the private
container registry of a private organisation (CI variable `ROM_PKG_OWNER`). This repo only **pulls** that image.

| File | Role |
|---|---|
| `ci/qemu-image.txt` | The image used here, pinned by digest: `qemu-eos@sha256:…` (registry host = the CI server, owner = `ROM_PKG_OWNER`) |
| `ci/qemu/pull-image.sh` | Pulls it on the CI host (`ROM_TOKEN` as read:package credential, throw-away authfile) |
| image `/opt/qemu-eos` | `qemu-system-arm`, `qemu-img` (arm-softmmu, TCG plugins, VNC, no GUI); `plugins/libwakeups.so` (idle/IRQ counters, source in `src/plugins/`); upstream `libmagiclantern.so` (DryosDebugMsg); `QEMU_EOS_SHA` (source commit), `BUILD_PROFILE`; gcc, arm-none-eabi binutils, mtools, python3-pil |
| `ci/qemu/make_sd_image.py` (+ `test_sd_image.sh`) | Raw SD image from `platform/sd.qcow2.xz` + an ML zip (qemu-img + mtools; no guestmount/FUSE) |
| `ci/qemu/scenario.py` | Boots one scenario, takes screenshots, collects logs and plugin output |
| `ci/qemu/shoot.py` (+ `card.py`) | Shooting scenarios: pictures taken by Canon and ML, read back from the card (see *Shooting scenarios*) |
| `ci/qemu/run_cam.sh` (`run_eosm.sh`) | All scenarios for one camera (in parallel by default) + comparison table; `SKIPPED` without ROMs |
| `ci/qemu/fetch_roms.sh`, `ci-emulation.sh`, `check_rom_md5.sh` | CI side of the ROM handling below |
| `tools/upload_roms.sh` | Maintainer helper: upload a camera's ROMs to the private package |

Moved to QEMU-EOS-Optimized: the Containerfile, `plugins/wakeups.c`, `build-plugin.sh` and the bare-metal plugin
self-test. Its `docs/PERFORMANCE.md` holds the benchmarks (boot-to-GUI, MIPS, plugin overhead, build flags).

**Updating QEMU:** merge/push in QEMU-EOS-Optimized; its CI run prints, in the "Publish image" step, the line to
put into `ci/qemu-image.txt` (`…/qemu-eos@sha256:<digest>`; tags `:<commit>` and `:main` exist too, but CI here
only accepts a digest, so a re-pushed tag can never change what we test with). Push here and check that the
`emulation` job is green and the comparison table did not move.

## Cameras and scenarios

`CAM` selects the camera as qemu-eos names it; the ML platform is `platform/CAM.<fw>`
(`EOSM` → `EOSM.202`, `1100D` → `1100D.105`, `4000D` → `4000D.110`). CI runs `EMU_CAMS="1100D EOSM 4000D"`.
The 1100D (DIGIC 4) is the maintainer's second body and validates the whole chain before EOS M ROMs exist.
The EOS M (DIGIC 5) boots Canon's GUI in qemu-eos (`hw/eos/cams/EOSM`) and runs the `canon`, `ml-full` and
`ml-photo` scenarios; it starts in LiveView with a muted grey screen, so its ready marker is a log line
(`startupInitializeComplete`), and it has no deep-sleep model and no capture emulation yet (no shooting scenarios).
The 4000D (DIGIC 4+) is the port in progress (`PORT-4000D.md`): before its scenarios, `run_cam.sh` checks every
stub against the ROM (`tools/check_stubs.py`, result in `stubs-check.txt`), and its `canon` scenario ends with a
key probe (`scenario.py --probe-keys`, `PROBE_KEYS`): each emulated key is sent once, the GUI events Canon logs for
it go to `keys.json` with a screenshot per key, which is how `gui.h` is checked. Its ML scenarios also walk the ML
menu (`scenario.py --menu-tour`, `MENU_TOUR`: the menu is reopened after the config check and `right` moves to the
next tab, `menu_tourNN_right.png`; every key must change the screen, then MENU closes it and the holds must be back
to 0), and the photo scenarios switch modules on (`PHOTO_MODULES`: an empty `ML/SETTINGS/<NAME>.EN` per module, so
ML loads it at boot; default `file_man img_name lua`, `sd_clock` stays off).

| Scenario | Card | Camera bootflag | Meaning |
|---|---|---|---|
| `canon` | base image, no ML | off (`firmware=boot=0`) | Canon firmware idle baseline |
| `ml-full` | `ML_PROFILE=full` zip (1100D: the only build) | on | upstream-equivalent ML |
| `ml-photo` | `ML_PROFILE=photo` zip (EOSM, 4000D) | on | Penumbra's photo build (counted powersave holds) |
| `ml-hold` | `make POWERSAVE_HOLD=y` zip (1100D) | on | `ml-full` with counted powersave holds (`analysis/powersave-hold.md`) |
| `ml-hold-ivl` | `ml-hold` zip + config preset `ML/SETTINGS/MAGIC.CFG` (1100D) | on | intervalometer auto-started at boot (first shot after 15 min): an active hold |
| `ml-photo-ivl` | `ml-photo` zip + the same preset (4000D, `IVL_PHOTO=1`) | on | the photo build's intervalometer: its hold must be taken and held |

Timeline per scenario (host time): boot `WARMUP_S` (40 s, not measured) → screenshot `boot.png` →
measure `MEASURE_S` (30 s) → screenshot `idle.png` → `quit`.
Deep sleep (1100D and 4000D, `DISPLAY_OFF_S`, default 30): after the window, INFO (`sendkey i`) turns the display off for
`DISPLAY_OFF_S` → `display_off.png`, then INFO again wakes the camera → `woken.png`. The segment reports Canon's
`pmSelfRefresh : In/Out`, the emulator's `[PM1100D]` / `[PM4000D] self-refresh` lines (length of each sleep, wake interrupt),
the plugin's halted time and rates, task switches and the counter samples.
For ML scenarios, after that, the Trash key (`sendkey delete`) opens the ML menu → `menu.png`; MENU dismisses the
beta warning (`menu_main.png`: the menu itself), SET selects an entry and MENU twice closes the menu, so ML saves its config (`menu_closed.png`;
`run_cam.sh` copies the card's `ML/SETTINGS` listing and `MAGIC.CFG` into `sd_settings.txt`). `compare.py` prints
the deep-sleep table and fails the job (`DEEP_SLEEP_ASSERT=1`) unless `canon` and `ml-hold` (4000D: `ml-photo`) sleep, `ml-full`
and `ml-hold-ivl` / `ml-photo-ivl` never do (Canon counter ≥ 1 in every sample), the screen changes on the wake key, the ML menu opens,
the config is written intact and its `fio` holds pair, and ML tasks keep running in `ml-hold-ivl`.
Wakeup metrics in the table are cut from the plugin's 1 s interval records for the window only.
`comparison.md` also reports, per scenario, Canon's last power-save counter (`[PM] … (Counter = n)`), ML's last hold
total (`[PS] … -> n`) and the number of `pmSelfRefresh` messages.
It also gives Canon's counter changes per second in the window (one `[PM]` line per change), the `[PM]` calls
while the ML menu is open (`--menu-s`, 6 s) and, for the 1100D and 4000D, samples of the counter in RAM (0x2B64,
0x3164C; `docs/firmware/1100D-powersave.md`, `docs/firmware/4000D-consts.md`) read through the monitor every ~0.5 s.
With no `[PM]` call in the window every sample must equal the last logged count, else the scenario fails (a wrong
counter address shows up there).
Outputs per scenario in `emu-out/<CAM>/<scenario>/`: `wakeups.json`, `tasks.json`, `result.json`, `qemu.log`
(DryosDebugMsg + task switches), screenshots; `emu-out/<CAM>/comparison.md` holds the table.

What stays here uses this tree (platform dirs, stubs, `sd.qcow2.xz`, ML builds, ROM checksums); everything that
only depends on QEMU is in QEMU-EOS-Optimized.

## Adding a camera

Everything that differs per camera is in two places, so that `run_cam.sh`, `shoot.py` and `compare.py` hold no
`case "$CAM"` logic:

1. **`platform/<CAM>.<fw>/emulation.sh`** in this repo (shell, sourced by `run_cam.sh` after it finds the platform
   dir; every value is `VAR=${VAR:-default}` so an environment variable still overrides it; comment each value with
   its evidence, e.g. `docs/firmware/1100D-powersave.md`). Copy `platform/1100D.105/emulation.sh` and set:
   - `PM_COUNTER_ADDR`: RAM address of Canon's power-save counter (empty: not sampled);
   - `DISPLAY_OFF_S`: length of the display-off (deep-sleep check) segment, 0 for none; with it `ml-hold-ivl` runs;
   - `CANON_READY`: DryosDebugMsg marker that times the `canon` window (empty: from launch);
   - `SHOOT_SCENARIOS`, `SHOOT_CANON_READY`, `SHOOT_SHUTTER_KEY`, `SHOOT_NO_FILES`: the shooting scenarios of the
     camera (empty: none), their ready marker, the qemu-eos key of a full press, scenarios checked without files;
   - `IVL_PHOTO` (1: `ml-photo-ivl`), `PROBE_KEYS`, `MENU_TOUR`, `PHOTO_MODULES`: port bring-up options;
   - `EXPECT_SLEEP`: deep-sleep expectations that differ from the generic ones in `compare.py` (`ml-photo=1`).
   A platform without the file runs with the generic defaults of `run_cam.sh`.
2. **The QEMU side** lives in QEMU-EOS-Optimized, not here: the machine profile in `hw/eos/cams/<CAM>/` and its
   description in `docs/models/PROFILES.md` there. A camera needs both before CI can emulate it; then add it to
   `EMU_CAMS` (default in `ci/qemu/ci-emulation.sh`), upload its ROMs and checksums (see *ROM handling*) and bump the image
   pin in `ci/qemu-image.txt` if the QEMU side changed.

## Shooting scenarios (photo capture)

The pinned QEMU image emulates the shutter release on the 1100D and the 4000D (QEMU-EOS-Optimized
`hw/eos/capture.c`, `docs/models/capture.md` there): a full shutter press or a remote release runs Canon's
whole shooting sequence and the picture lands on the emulated card, with Canon's EXIF. `run_cam.sh` then runs
`ci/qemu/shoot.py` for each scenario of the camera (`SHOOT=0` skips them; the camera specifics, readiness
marker, shutter key and scenario list, are `SHOOT_*` values in the platform's `emulation.sh`, see *Adding a camera*):

| Scenario | Trigger | Checked |
|---|---|---|
| `canon-shot` | Canon only: full press (left Ctrl) | 1 release, 1 picture with EXIF; what Canon captured (`iso:..,tv:..,av:..`) equals what the MPU reported |
| `ml-ivl` | ML intervalometer from a config preset (5 s, 3 shots, started at boot) | 3 releases through ML's remote release (`PROP_REMOTE_SW2`), 3 pictures |
| `ml-hdr` | ML HDR bracketing (3 frames, 2 EV) from 1/100 s, one press | Tv of the 3 releases = base, base -2 EV, base +2 EV (raw -16 / 0 / +16) as reported by the MPU and captured by Canon; 1100D also 3 pictures with 3 different EXIF exposure times (4000D: releases only, see below) |
| `ml-bulb` | ML `bulb_take_pic` (intervalometer in BULB, 3 s) | 1 release in BULB (Tv 0x0C), bulb end sent, 1 picture |

Per shot, the exposure ML asked for is visible three ways: the emulator's `[CAPTURE] shot n: tv=.. av=.. iso=..
ae=.. mode=..` (the values the DIGIC last sent to the MPU), Canon's own `iso:..,tv:..,av:..` when it starts the
capture, and the EXIF of the file (`card.py` reads the FAT card image after QEMU exits; `result.json` lists
both, and the table goes into `comparison.md`). The scenarios are anchored to the guest (ready markers, then
the file jobs Canon's file writer completes, `fwCompleteJob`; its "write done" line is printed only when the
write took a tick of its timer); no monitor or no ready marker is an `INFRASTRUCTURE TIMEOUT`
(exit 3, retried once), missing pictures once the guest runs are a failure. A starting shutter speed (`ml-hdr`,
`ml-bulb`) is reported by the emulated MPU once the GUI is up, as if the dial had been turned, so Canon, ML and
the MPU agree on it.

What this proves: ML's shooting features drive Canon's real capture code end to end (release requests, the
exposure values ML sets reach Canon's capture and the file, HDR steps, intervalometer count, bulb start/end,
files written in order). What it does not: no image processing (the sensor data is a fixed dark pattern, the
JPEG a uniform gray frame whose level follows the exposure, the CR2 raw data zeros), timing is not physical
(exposure length = Canon's VD count at the emulator's period; a bulb's EXIF time is not the requested one;
intervals are guest time on a loaded host), no metering (auto modes use what the MPU last reported), single
frames only (no continuous drive, LiveView capture, flash, mirror lockup). On the 4000D, exposures over about a
second take a long-exposure correction path that is not always completed, hence the 1/100 s HDR base, and
developing back-to-back frames to files stalls in about one run in three (emulated engine, not ML), so its
`ml-hdr` checks the releases and Canon's captures but not the files (`SHOOT_NO_FILES` in `platform/4000D.110/emulation.sh`). The
bulb *timer* trigger (half-shutter held in BULB) is not exercised: it needs ML's display-idle state with the
display on, which Canon turns off on half press.

## Metrics and their limits

From `libwakeups.so` (no ROM-specific addresses, so the same plugin serves every camera and the self-test; all its
counters are inline TCG adds emitted at translation time, so it does not slow the guest measurably):

- **wakeups/s**: executions of `mcr p15,0,Rd,c7,c0,4` (the ARM926/946 wait-for-interrupt DryOS uses in its
  idle task), plus the other pre-v7 WFI encodings. Each idle entry ends with one wakeup.
- **IRQ/s**: executions of the IRQ vector (0x18, or 0xFFFF0018 with high vectors). FIQ counted separately.
- **insns/wakeup**: instructions executed between two consecutive WFIs, i.e. work per wakeup (mean; the max is
  reported only with the plugin option `burst=on`, which ML's scenarios do not set). Counted per translation block, exact unless a block is cut short by a synchronous exception.
- **busy ratio**: `1 - halted/elapsed` in host time. TCG runs slower than the real CPU, so this
  over-estimates busy time; compare it only between scenarios on the same runner. With `icount_shift=N`
  (and `-icount shift=N`) it is `insns·2^N ns / elapsed` instead; `-icount` forces single-threaded TCG,
  where QEMU 4.2 reports no idle events (halts stay 0).
- **task switches/s** (`-d tasks`, needs `.current_task_addr` in qemu-eos's model): DryOS context switches
  in the window, per task in `tasks.json`. The PLAN's per-task wakeup metric.

Limits: QEMU has **no power model**. Numbers are **relative** (scenario vs `canon` baseline, same runner,
same window), never absolute battery estimates. Without `-icount`, guest time follows host time, so a
loaded runner can shift rates a little; run twice before trusting a small delta. Peripherals are partial:
no LiveView sensor, partial MPU/display emulation, so LiveView-driven tasks may behave differently than on
hardware. Hardware (phase 8) is the final check.

`scenario.py` also samples the guest PC through the monitor (`info registers`, every 0.5 s) during the window;
the comparison lists the hottest PCs. This ROM-independent profile shows where an idle CPU really spends its time.

## Findings so far (1100D.105, CI run 20)

| scenario | WFI/s | IRQ/s | task sw/s | busy ratio | hottest PC |
|---|---:|---:|---:|---:|---|
| canon | 0 | 103.8 | 68 | 1.00 | 0xFF1EA818 (98%) |
| ml-full | 107.8 | 111.5 | 357 | 0.02 | 0xFF1EA9B4 (98%, right after the WFI) |

What works: Canon boot to the shooting screen (Q screen screenshot), ML 1100D.105 boot (overlay, "Magic Lantern"
first-run screen via the Trash key), DryosDebugMsg logging, task-switch logging, the plugin, ROM fetch + checks.

What does not (qemu-eos limitations, not ML bugs):
- **Fixed since (QEMU-EOS-Optimized `eos_pm_1100d`, `emu/deep-sleep`; 4000D: `hw/eos/cams/pm_4000d.c`, `emu/4000d-pm`):** Canon idle now takes the low-clock WFI
  with the display on and the `selfRefresh` deep sleep with the display off (`analysis/powersave-hold.md`, *QEMU
  results: deep sleep*). The note below describes the images before that model.
- **Canon idle does not sleep in emulation (old images).** The DIGIC 4 idle loop (around 0xFF1EA7E0) only executes its WFI
  (`mcr p15,0,r0,c15,c8,2` at 0xFF1EA9B0) when power save is *disabled*. With power save enabled (Canon alone:
  `[PM] EnablePowerSave (Counter = 0)`) it polls power-management hardware that qemu-eos does not emulate, so the
  CPU spins at 0xFF1EA818. ML keeps power save disabled (counter 2-3), so the idle loop takes the WFI path.
  Consequence: for the 1100D, **WFI/s and busy ratio are not comparable between `canon` and ML**. IRQ/s and task
  switches/s are (ML adds ~290 switches/s, `audio_common_task` alone ~100/s, matching `analysis/power-audit.md`).
  Comparisons between ML builds (full vs photo) are valid on every metric, since both take the same idle path.
- **ROM0 is not mapped** for the 1100D in qemu-eos: ML's first-boot ROM backup reads 0xF0000000 and QEMU aborts
  (`io_readx: Assertion`). `run_cam.sh` seeds zero-filled `ML/LOGS/ROM0.BIN`/`ROM1.BIN` of the right size so
  ML skips the backup (which would also distort the window).
- No LiveView (the 1100D needs a key press to enter it; the sensor path is not emulated anyway).

## Running

### CI

The `emulation` job pulls the pinned image (seconds; no QEMU build here any more) and runs the SD tooling test.
Camera scenarios run only when that camera's ROMs are in the package; otherwise the step prints
`SKIPPED: no ROM for <CAM>` and succeeds. A camera's scenarios run at once when the runner is not loaded
(`PARALLEL=auto`: 1-min load average + scenarios <= 1.5 x CPUs; `PARALLEL=1`/`0` to force), so the camera takes
one timeline instead of one per scenario; on a loaded runner they run one after the other.
The timeline starts when QEMU's monitor socket appears (`qemu_start_s` in `result.json`), not when QEMU is
launched: the plugin times its window from its own load, and a cold start of the freshly pulled binary on the busy
runner took up to ~12 s, which used to shorten the 30 s window (to 18-25 s in runs 34, 38, 40).
`plugin_clock_lag_s` (quit time vs the plugin's last timestamp) checks that both clocks agree.

**Robust under load.** Every phase is anchored to the guest, not to a fixed delay (run 55: QEMU took 32 s to
come up and the boot was so slow that the "40 s" window measured a half-booted camera: no ML yet, no monitor
samples, ML holds `None`, deep-sleep checks failed on `ml-hold`):
- *ready*: the window starts at `max(WARMUP_S, ready + 10 s)`, where ready is the DryosDebugMsg log showing
  `TurnOnDisplay finish` (1100D Canon GUI up) and, for ML builds, `[PS] release boot` (ML's init over); other
  models use two identical non-blank screenshots. `ready_s` / `window_start_s` are in `result.json`; on an idle
  runner the window is where it always was (40 s).
- *display off* is timed from Canon's `TurnOffDisplay` log line after the key; *wake* and *ML menu* screenshots
  are retaken (up to 60 s) until the screen changes; the config save waits for its FIO release.
- the monitor client waits for QEMU's prompt before sending a command and retries the connection.
- missing evidence is never a pass: too few monitor samples (< 1/4 of one per 0.5 s), a screendump that never
  completes, no ready marker within 300 s or no monitor within 180 s end the scenario as
  `INFRASTRUCTURE TIMEOUT` (exit 3, distinct from a functional failure). `run_cam.sh` reruns such a scenario
  once, alone; a second timeout fails the job as infrastructure. A missing `[PM]`/`[PS]` value at the window
  end, an empty counter sample set or a scenario without window rates is a failure (`compare.py` exit 1).
Results are uploaded as artifact `ml-optimized-emulation-<run number>` (tables, JSON, logs, screenshots; never
ROMs, disk images or binaries). Download it with the link the upload step prints (`download: ...`): Forgejo's
artifact download route `/actions/runs/<N>/artifacts/<name>` takes the run *id*, not the run number shown in the
run's URL. Before the names carried the run number, the run number in that route fetched another run's artifact
of the same name (older, without the 4000D results); now it is a 404.

### Locally

Needs podman (the CI runner has it). With podman and a `read:package` token:

    podman login --tls-verify=false <forgejo-host>             # token as password
    IMG=$(grep -v '^#' ci/qemu-image.txt); podman pull --tls-verify=false "$IMG"
    podman run --rm -v "$PWD":/work -w /work "$IMG" ci/qemu/test_sd_image.sh emu-out/sd-tooling
    # camera run: ROMs in ~/roms/<CAM>/, ML zip built with ci/build.sh or make
    podman run --rm --tmpfs /tmp -v "$PWD":/work -v ~/roms:/roms:ro -w /work \
        -e CAM=1100D -e ML_ZIP_FULL=/work/platform/1100D.105/build/magiclantern.zip "$IMG" ci/qemu/run_cam.sh

Interactive use (GUI via VNC): run `qemu-system-arm` from the image with `-vnc :1` instead of
`-display none` (see the command `scenario.py` prints), and send keys through the monitor (`sendkey`).

### Capture kit (EOS M)

`ci/qemu/capture_kit.py` boots the read-only capture kit (`ML_PROFILE=capture`, `docs/CAPTURE-KIT.md`) in qemu-eos
with `ML/CAPTEST.TXT` on the card, lets it run every capture once, and fails on a hang, a missing capture file,
a refused write, any serial flash program/erase, any `WREN`/`WRSR` outside Canon's start-up task, or any ROM write
(`-d sflash,romw`). It is self-contained and does not need the `run_cam.sh` machinery. It needs `ROM0.BIN`,
`ROM1.BIN` and `SFDATA.BIN` in `<rom-dir>/EOSM/` (read only; copied to a scratch dir) and, to build the card,
mtools and `platform/sd.qcow2.xz` (both in the qemu-eos image):

    (cd platform/EOSM.202 && make ML_PROFILE=capture FATAL_WARNINGS=y)
    podman run --rm --tmpfs /tmp -v "$PWD":/work -v ~/roms:/roms:ro -w /work "$IMG" \
        python3 -I ci/qemu/capture_kit.py --zip platform/EOSM.202/build/magiclantern.zip \
        --rom-dir /roms --out emu-out/capture-kit

Without the image: set `QEMU_EOS` to a prefix holding `bin/qemu-system-arm` and `plugins/libmagiclantern.so`, and
pass a prepared card with `--sd IMG --fat-offset BYTES` (it must hold `autoexec.bin`, the zip's `ML/` and
`ML/CAPTEST.TXT`). It finishes in about two minutes. Results: `capture-kit.txt` (card files, serial flash opcode
counts, problems), `qemu.log`, and the kit's `CAPTEST.OK`/`BOOTINFO.TXT`/`LVINFO.TXT`/`PM00.LOG`.
Exit codes: 0 pass (or SKIPPED without ROMs), 1 check failed, 3 hang / emulator never got there.
The CI `emulation` job does not run it yet (it is added with the EOS M emulation job).

## ROM handling

ROM dumps (ROM0.BIN, ROM1.BIN, SFDATA.BIN) are Canon firmware read from the maintainer's own camera.
They are **never** committed, put in artifacts, printed in logs, or stored in images.

- **Storage**: a Forgejo generic package `canon-rom` in a private package registry, version
  `<CAM>-<FW>` (`1100D-105`, `EOSM-202`). Packages inherit their owner's visibility, so the owner must be a
  **private** organisation; a package under a public user or org would be anonymously downloadable. The
  owner's name is the CI variable `ROM_PKG_OWNER`; the server is the CI server itself (`GITHUB_SERVER_URL`).
- **Checksums**: `docs/firmware/rom-md5.txt` (`<CAM>-<FW> <FILE> <md5>`), safe to commit. CI verifies the
  sha256 the server reports and these MD5s; a mismatch fails the job.
- **Upload** (maintainer, local): `ROM_PKG_OWNER=<private org> tools/upload_roms.sh 1100D <dir with the dumps>`
  (`--replace` to overwrite). Credentials come from the environment or from a file named by
  `FORGEJO_CREDENTIALS`, kept outside the repository. It refuses a public owner, verifies the stored sha256
  and prints the lines for `rom-md5.txt`.
- **CI access**: repo secret `ROM_TOKEN`, a personal access token with only the `read:package` scope, from a
  user that is a member of the private owner org; and repo variable `ROM_PKG_OWNER` (the org's name). Create
  the token in Forgejo: user settings → Applications → Generate token → scope `package: read`; then repo →
  Settings → Actions → Secrets → add `ROM_TOKEN`, and Variables → add `ROM_PKG_OWNER`. Without the token the
  job token is tried, which cannot read another owner's private packages; without the variable nothing is
  fetched. In both cases the scenarios are SKIPPED. The same token pulls the
  QEMU image, which is published as a private package of the same owner.
- **In the job** (`ci-emulation.sh`, on the runner host): ROMs go to a fresh `mktemp -d /dev/shm/ml-roms.*`
  (tmpfs, mode 700), the token reaches curl through a mode-600 header file (never argv), the directory is
  bind-mounted **read-only** into the sandboxed container (`--network=none`, no capabilities), and is deleted
  by an exit trap. Disk images (ML writes a ROM backup to the card on first boot) live in the container's
  tmpfs `/tmp` and disappear with it; a final sweep deletes any `*.bin`/`*.img`/large file from the results.
