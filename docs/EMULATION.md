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
The 1100D (DIGIC 4) is the maintainer's second body and validates the whole chain before EOS M ROMs exist.

| Scenario | Card | Camera bootflag | Meaning |
|---|---|---|---|
| `canon` | base image, no ML | off (`firmware=boot=0`) | Canon firmware idle baseline |
| `ml-full` | `ML_PROFILE=full` zip (1100D: the only build) | on | upstream-equivalent ML |
| `ml-photo` | `ML_PROFILE=photo` zip (EOSM only) | on | Penumbra's photo build |

Timeline per scenario (host time): boot `WARMUP_S` (40 s, not measured) → screenshot `boot.png` →
measure `MEASURE_S` (30 s) → screenshot `idle.png` → `quit`.
For ML scenarios, after the window, the Trash key (`sendkey delete`) opens the ML menu → `menu.png`.
Outputs per scenario in `emu-out/<CAM>/<scenario>/`: `wakeups.json`, `tasks.json`, `result.json`, `qemu.log`
(DryosDebugMsg + task switches), screenshots; `emu-out/<CAM>/comparison.md` holds the table.

QEMU side vs ML side: `ci/qemu/Containerfile`, `ci/qemu/plugins/`, `ci/qemu/build-plugin.sh` and
`ci/qemu/selftest/` (except `test_sd_image.sh`) only depend on QEMU and can move to a separate QEMU fork;
the rest uses this tree (platform dirs, stubs, `sd.qcow2.xz`, ML builds, ROM checksums).

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
- **Canon idle does not sleep in emulation.** The DIGIC 4 idle loop (around 0xFF1EA7E0) only executes its WFI
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

The `emulation` job always runs the plugin self-test. Camera scenarios run only when that camera's ROMs
are in the package; otherwise the step prints `SKIPPED: no ROM for <CAM>` and succeeds.
Results are uploaded as artifact `ml-optimized-emulation` (tables, JSON, logs, screenshots; never ROMs,
disk images or binaries).

### Locally

Needs podman. With podman:

    podman build --network=host -t ml-qemu -f ci/qemu/Containerfile ci/qemu
    podman run --rm -v "$PWD":/work -w /work ml-qemu ci/qemu/selftest/selftest.sh
    # camera run: ROMs in ~/roms/<CAM>/, ML zip built with ci/build.sh or make
    podman run --rm --tmpfs /tmp -v "$PWD":/work -v ~/roms:/roms:ro -w /work \
        -e CAM=1100D -e ML_ZIP_FULL=/work/platform/1100D.105/build/magiclantern.zip ml-qemu ci/qemu/run_cam.sh

Interactive use (GUI via VNC): run `qemu-system-arm` from the image with `-vnc :1` instead of
`-display none` (see the command `scenario.py` prints), and send keys through the monitor (`sendkey`).

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
  fetched. In both cases the scenarios are SKIPPED.
- **In the job** (`ci-emulation.sh`, on the runner host): ROMs go to a fresh `mktemp -d /dev/shm/ml-roms.*`
  (tmpfs, mode 700), the token reaches curl through a mode-600 header file (never argv), the directory is
  bind-mounted **read-only** into the sandboxed container (`--network=none`, no capabilities), and is deleted
  by an exit trap. Disk images (ML writes a ROM backup to the card on first boot) live in the container's
  tmpfs `/tmp` and disappear with it; a final sweep deletes any `*.bin`/`*.img`/large file from the results.
