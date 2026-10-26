# EOS M capture kit (`ML_PROFILE=capture`)

A read-only build of the EOS M (firmware 2.0.2) that collects data from a real camera for the emulator
(qemu-eos, `docs/EMULATION.md`): what Canon's firmware does and says at boot, in LiveView, in playback, in the
menus and in power save, and what the hardware returns to it. It changes nothing in the camera.

It is not a feature build. It has its own, tiny feature set and no way to write to the camera's persistent state.

## What it captures

Everything is written to `ML/LOGS/` on the card.

| Capture (menu "Capture")           | Files                                   | Use                                              |
|------------------------------------|------------------------------------------|--------------------------------------------------|
| Boot logs (automatic from power-on)| `BOOTDM.LOG`, `BOOTMPU.LOG`, `BOOTINFO.TXT` | Canon's `DryosDebugMsg` stream and all MPU messages (both directions) from the first instruction after `init_task`; kept in RAM, saved on request |
| Dump serial flash                  | `SFDATA.BIN`                            | 8 MiB, the raw layout the emulator replays (settings and calibration packages; the camera does not start in the emulator without it) |
| Logging session                    | `DMnn.LOG`, `MPUnn.LOG`, `PMnn.LOG`     | `DryosDebugMsg` + MPU traffic + power-management samples (PowerMgr counter and mode, clock enable and mask, second-core clock/reset registers, LiveView and display state) over a chosen time, while the user presses keys, opens menus, plays back, turns the display off |
| Dump LiveView buffers              | `LVn.422`, `HDn.422`, `LVINFO.TXT`      | YUV422 frames and where they live         |
| RAM dump (label + countdown)       | `RAM_<label>.BIN`, `.TXT`               | 256 MiB of DRAM in a state the user prepared during the countdown (LiveView, menu, playback, display off, other) |

Log lines are `seconds.microseconds task class level text`; MPU lines are `time S|R hex bytes` (`S` = sent to the
MPU, `R` = received).

## Safety design

The kit must not be able to change settings, firmware, boot flags, the serial flash or the ROM. Four independent
layers; the last two are mechanical.

1. **No write paths are assembled.** `platform/EOSM.202/features-capture.h` replaces the whole feature set;
   `internals-capture.h` switches off every `CONFIG_*` that implies a write (property writes, custom functions,
   engine registers, modules, boot flag, ROM backup, firmware update, display filters, FPS/exposure overrides, ...).
   `src/capture-kit.c` refuses to compile if one is enabled (`#error`).
2. **Write-capable stubs are not defined.** The routines that write (`prop_request_change`, `prop_deliver`,
   `SetCFnData`, `EngDrvOut`, `engio_write`, audio codec writes, the serial flash program/erase routines, MPU
   send, ...) are not in the capture `stubs.S`. ML's own `prop_request_change`, `_EngDrvOut` and `_engio_write` are
   logging no-ops that count the attempt (`[CAPTURE] blocked ...` in the debug log, `blocked=` in `CAPTEST.OK`).
3. **Build-time check, `tools/check_capture_kit.py`** (run by `make`; the build fails if it fails). It scans the
   linked ELF, `autoexec.bin` and the zip against `platform/EOSM.202/capture-forbidden.txt`, a list of
   `<address> <name> <reason>` lines for every Canon routine the kit must never reach, and of eventproc names:
   - every 32-bit word of the loaded sections (literal pools, function pointers, tables), with ROM addresses also in
     their `0xF8..0xFF` aliases;
   - every ARM `B`/`BL`/`BLX` target in executable sections;
   - the symbol table (a stub named like a forbidden routine, or any symbol with a forbidden value);
   - every NUL-terminated string (Canon's `call("name")` looks eventprocs up by name);
   - the zip: only `autoexec.bin` and `ML/` data, no firmware update file, no modules.
   The only exception is the read-only signature table that verifies the firmware before the kit reads from it
   (listed as "allowed" in the check output). The list may only be loosened with a written reason in the same
   commit. The check output is `build/capture-check.txt`.
4. **Emulation self-test** (below): the kit runs every capture in qemu-eos with the serial flash and ROM write
   logging on, and the run fails on any flash write/erase command, ROM write, or refused write attempt.

What the kit does change, deliberately and minimally:
- one instruction of `DryosDebugMsg` in RAM (a branch to the kit's logger), checked against the expected original
  first, and undone when logging stops;
- Canon's `DisablePowerSave` / `EnablePowerSave` counter while a file is being written (a counted hold, released
  when the write ends), so the camera cannot enter deep sleep in the middle of a card write;
- files on the card.

Reads of the camera: the serial flash only through Canon's own READ routine (opcode `0x03`, never `WREN`), the MPU
rings and the power-management words are read with plain loads (the second-core mailbox FIFOs, where a read pops,
are never touched). Every firmware address the kit uses is verified against its expected instruction words at run
time ("Kit status" in the menu shows the result); on a mismatch the capture that needs it refuses to run.

Nothing is installed in the camera: the card carries `autoexec.bin` and `ML/`; removing the card restores the
original behaviour. A card that boots the kit needs the usual boot flag on the card (not in the camera).

## Building

    . <environment with the Arm GNU toolchain 12.2, see CONTRIBUTING.md>
    cd platform/EOSM.202 && make ML_PROFILE=capture FATAL_WARNINGS=y

Result: `platform/EOSM.202/build/magiclantern.zip` (`autoexec.bin` + `ML/` data) and
`build/capture-check.txt`. The build is only valid for firmware 2.0.2 (`#error` otherwise).

## Emulation test

`ci/qemu/capture_kit.py` boots the kit in qemu-eos with `ML/CAPTEST.TXT` on the card. With that file the kit
runs the self-test: boot logs, serial flash dump, a 20 s session, LiveView buffers and a 16 MiB RAM dump, then logs
`[CAPTURE] selftest done errors=N blocked=M` and writes `ML/LOGS/CAPTEST.OK`. The script fails when:

- the self-test does not finish (hang), `errors` or `blocked` is not 0, or a capture file is missing or too small;
- qemu-eos' serial flash model (`-d sflash`) sees any page program or erase opcode, an opcode it does not know, or
  a `WREN`/`WRSR` outside Canon's own start-up task (Canon writes the flash status register twice on every boot; the
  same two commands appear when the card carries no ML at all);
- any write to a ROM region is logged (`-d romw`);
- the `SFDATA.BIN` the kit saved differs from the flash image the emulator loaded.

See `docs/EMULATION.md`, "Capture kit", for how to run it. In the emulator LiveView has no sensor frames, so the
LiveView buffer dump finds no buffer (`LVINFO.TXT` records `0x00000000`) and the MPU session log stays short; the
code paths are exercised, the data is only real on the camera.

## Limits

- The boot log lives in RAM (2 MiB when the heap allows it, 384 KiB at the very start): the first few thousand
  lines of a boot, written before the kit's own initialisation, can be lost; the count is in `BOOTINFO.TXT`.
- Debug-message class 21 (engine register writes, one line per write) is not logged: it floods the buffer.
- The sensor's ADTG/CMOS register tables (class 0x91 level 1, `[REG] ...`), which Canon prints again for every
  LiveView frame, are kept for the first 4000 lines of each log and then only counted (`BOOTINFO.TXT`, and the
  `session end` line): on the camera they filled a quarter of every 4 MiB session, which then stopped after
  6-28 s instead of the time chosen.
- ML's own menu dims Canon's bitmap palette (512 `EngDrvOut` per redraw). The kit blocks them like any other
  write but logs one line per 512 (`blocked ... palette writes`); `BOOTINFO.TXT` gives the count.
- Canon's `snprintf` does not know `%u`, `%p` or width/precision on `%s`: the kit formats numbers with `%d`/`%x`
  and pads by hand. Keep that in mind when adding log lines.
