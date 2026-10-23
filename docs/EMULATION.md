# Emulation (qemu-eos)

Phase 3 of `docs/PLAN.md`: boot the Canon firmware (and ML) headless in QEMU and measure how often the
CPU leaves idle. Everything lives in `ci/qemu/`; CI runs it in the `emulation` job of
`.forgejo/workflows/ci.yml`.

## Pieces

| File | Role |
|---|---|
| `ci/qemu/Containerfile` | Image `ml-qemu:<hash>`: qemu-eos branch `qemu-eos-v4.2.1` pinned at `4b667a1d3c08ab7a55835d15ddbd884fa754946d` (QEMU 4.2.1), `arm-softmmu` with TCG plugins and VNC, no GUI; upstream `libmagiclantern.so` plugin (DryosDebugMsg); ML's `magiclantern/` tooling; gcc, arm-none-eabi binutils, mtools, python3-pil |
| `ci/qemu/plugins/wakeups.c` | TCG plugin: idle entries (WFI), IRQ/FIQ entries, instructions, halt time → JSON lines |
| `ci/qemu/selftest/` | Bare-metal ARMv5 program + checks: verifies the plugin on a generic machine, no ROM needed |
| `ci/qemu/make_sd_image.py` | Raw SD image from `platform/sd.qcow2.xz` + an ML zip (qemu-img + mtools; no guestmount/FUSE) |
| `ci/qemu/scenario.py` | Boots one scenario, takes screenshots, collects logs and plugin output |
| `ci/qemu/run_cam.sh` (`run_eosm.sh`) | All scenarios for one camera + comparison table; `SKIPPED` without ROMs |
| `ci/qemu/fetch_roms.sh`, `ci-emulation.sh`, `check_rom_md5.sh` | CI side of the ROM handling below |
| `tools/upload_roms.sh` | Maintainer helper: upload a camera's ROMs to the private package |

The image tag is the hash of `ci/qemu/Containerfile`, so QEMU (about 10 min) is rebuilt only when that file
changes. The plugin is compiled at job time (`build-plugin.sh`), so plugin changes cost seconds.

## Cameras and scenarios

`CAM` selects the camera as qemu-eos names it; the ML platform is `platform/CAM.<fw>`
(`EOSM` → `EOSM.202`, `1100D` → `1100D.105`). CI runs `EMU_CAMS="1100D EOSM"`.
The 1100D (DIGIC 4) is the user's second body and validates the whole chain before EOS M ROMs exist.

| Scenario | Card | Camera bootflag | Meaning |
|---|---|---|---|
| `canon` | base image, no ML | off (`firmware=boot=0`) | Canon firmware idle baseline |
| `ml-full` | `ML_PROFILE=full` zip (1100D: the only build) | on | upstream-equivalent ML |
| `ml-photo` | `ML_PROFILE=photo` zip (EOSM only) | on | this fork's photo build |

Timeline per scenario (host time): boot `WARMUP_S` (40 s, not measured) → screenshot `boot.png` →
measure `MEASURE_S` (30 s) → screenshot `idle.png` → `quit`.
Outputs per scenario in `emu-out/<CAM>/<scenario>/`: `wakeups.json`, `tasks.json`, `result.json`, `qemu.log`
(DryosDebugMsg + task switches), screenshots; `emu-out/<CAM>/comparison.md` holds the table.

## Metrics and their limits

From `wakeups.c` (no ROM-specific addresses, so the same plugin serves every camera and the self-test):

- **wakeups/s**: executions of `mcr p15,0,Rd,c7,c0,4` (the ARM926/946 wait-for-interrupt DryOS uses in its
  idle task), plus the other pre-v7 WFI encodings. Each idle entry ends with one wakeup.
- **IRQ/s**: executions of the IRQ vector (0x18, or 0xFFFF0018 with high vectors). FIQ counted separately.
- **insns/wakeup**: instructions executed between two consecutive WFIs, i.e. work per wakeup (mean; max also
  reported). Counted per translation block, exact unless a block is cut short by a synchronous exception.
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

## Running

### CI

The `emulation` job always runs the plugin self-test. Camera scenarios run only when that camera's ROMs
are in the package; otherwise the step prints `SKIPPED: no ROM for <CAM>` and succeeds.
Results are uploaded as artifact `ml-optimized-emulation` (tables, JSON, logs, screenshots; never ROMs,
disk images or binaries).

### Locally

Needs podman (not available on the dev host; use CI there). With podman:

    podman build --network=host -t ml-qemu -f ci/qemu/Containerfile ci/qemu
    podman run --rm -v "$PWD":/work -w /work ml-qemu ci/qemu/selftest/selftest.sh
    # camera run: ROMs in ~/roms/<CAM>/, ML zip built with ci/build.sh or make
    podman run --rm --tmpfs /tmp -v "$PWD":/work -v ~/roms:/roms:ro -w /work \
        -e CAM=1100D -e ML_ZIP_FULL=/work/platform/1100D.105/build/magiclantern.zip ml-qemu ci/qemu/run_cam.sh

Interactive use (GUI via VNC): run `qemu-system-arm` from the image with `-vnc :1` instead of
`-display none` (see the command `scenario.py` prints), and send keys through the monitor (`sendkey`).

## ROM handling

ROM dumps (ROM0.BIN, ROM1.BIN, SFDATA.BIN) are Canon firmware read from the user's own camera.
They are **never** committed, put in artifacts, printed in logs, or stored in images.

- **Storage**: Forgejo generic package `canon-rom` owned by the **private** org `<private-org>`, version
  `<CAM>-<FW>` (`1100D-105`, `EOSM-202`). The user `root` is public, and packages inherit their owner's
  visibility, so a package under `root` would be anonymously downloadable; anonymous access to
  `<private-org>` returns 401.
- **Checksums**: `docs/firmware/rom-md5.txt` (`<CAM>-<FW> <FILE> <md5>`), safe to commit. CI verifies the
  sha256 the server reports and these MD5s; a mismatch fails the job.
- **Upload** (maintainer, local): `tools/upload_roms.sh 1100D ~/Documents/roms/1100D` (`--replace` to
  overwrite). It uses `<credentials-file>`, refuses a public owner, verifies the stored sha256
  and prints the lines for `rom-md5.txt`.
- **CI access**: repo secret `ROM_TOKEN` on `root/ML-Optimized`, a personal access token with only the
  `read:package` scope, from a user that is a member of `<private-org>`. Create it in Forgejo: user settings →
  Applications → Generate token → scope `package: read`; then repo → Settings → Actions → Secrets → add
  `ROM_TOKEN`. Without it the job token is tried, which cannot read another owner's private packages, so the
  scenarios are SKIPPED.
- **In the job** (`ci-emulation.sh`, on the runner host): ROMs go to a fresh `mktemp -d /dev/shm/ml-roms.*`
  (tmpfs, mode 700), the token reaches curl through a mode-600 header file (never argv), the directory is
  bind-mounted **read-only** into the sandboxed container (`--network=none`, no capabilities), and is deleted
  by an exit trap. Disk images (ML writes a ROM backup to the card on first boot) live in the container's
  tmpfs `/tmp` and disappear with it; a final sweep deletes any `*.bin`/`*.img`/large file from the results.
