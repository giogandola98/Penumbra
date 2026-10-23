# Port: Canon EOS 4000D / Rebel T100 (fw 1.1.0)

Status: **planned** — starts after the development chain is complete on the 1100D (emulation, PM modeling,
powersave hold verified). Maintainer's own dump: ROM1 valid (MD5 matches the on-camera dumper), ROM0 is an
unpopulated region (repeated 16-byte bus pattern). No serial flash. Model ID 0x422, DryOS 2.3 r58+p7, ROMBASEADDR
0xFF0C0000. ROMs: private package registry, package `canon-rom`, version `4000D-110` (never in this repository).

Base platform: `1300D.110` (DIGIC 4+, armv5te, `boot-d45-ch.o`, same DryOS generation). Any upstream/community 4000D
work (forum threads, qemu-eos `2000d_test` branch) is checked first and reused only if the license allows (GPL-2.0).

## Steps
| # | Step | Verified by |
|---|---|---|
| 1 | **Emulator model** for 4000D in QEMU-EOS-Optimized (derive from 1300D: memory map, MPU spells, display, SD) | Canon firmware boots to GUI in QEMU; screenshot |
| 2 | **Firmware RE** with Ghidra: locate every stub listed in `platform/1300D.110/stubs.S` in the 4000D ROM (string/xref/signature matching vs 1300D patterns), plus consts (`consts.h`: VRAM, LV buffers, property IDs, GUI events) | Each stub has address + evidence in `docs/firmware/4000D-stubs.md`; automated check script compares stub signatures |
| 3 | **Minimal boot** (`minimal` / hello-world target) | Runs in QEMU, prints on screen |
| 4 | **Platform dir** `platform/4000D.110` (stubs.S, consts.h, internals.h, features.h minimal) | Builds with FATAL_WARNINGS=y; boots ML menu in QEMU |
| 5 | **Features** enabled incrementally (photo profile first: overlays, intervalometer, bracketing, powersave hold) | CI emulation tests per feature |
| 6 | **Boot flag**: no signed installer is needed. Canon's own `EnableBootDisk` routine, present in the 4000D ROM, is invoked over USB with MLinstall (GPL-2.0, github.com/petabyt/mlinstall), as on the 1300D; reported working on a 4000D fw 1.0.1. The flags are the first 16 bytes of ROM1 (FIR, BOOT, RAM, UPD; currently 0, 0, -1, -1). First emulate the flag write in QEMU to see exactly which flash range Canon erases and rewrites | QEMU trace of the flash write; then on hardware with the maintainer: read-only device info, one `EnableBootDisk` (full battery, menu not LiveView, no card I/O), dumper re-run shows BOOT set |
| 7 | **Hardware** (last, maintainer): read-only smoke test first, then features | Maintainer sign-off |

## Risks
- Enabling the camera boot flag is the only step that writes to the camera (one Canon flash-block rewrite). Low but
  non-zero risk: a power loss during that write could corrupt the block; never in LiveView or during sensor cleaning.
  It is reversible over USB (`DisableBootDisk`). A bad build on a bootable card only hangs at power-on (remove battery
  and card to recover); it does not brick the camera.
- Stubs found by pattern matching can be wrong in subtle ways; each must be validated in emulation before use.
