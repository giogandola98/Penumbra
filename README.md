Penumbra
========

Penumbra is a photo-focused, battery-efficient build of
[Magic Lantern](https://github.com/reticulatedpines/magiclantern_simplified) for the original
**Canon EOS M** (firmware 2.0.2).

**Penumbra is based on Magic Lantern.** It is a fork of `magiclantern_simplified` (the `dev` branch) by
the Magic Lantern developers, and nearly all of its code is their work. **It is not an official Magic
Lantern release, and the Magic Lantern developers do not endorse it.** Please do not report Penumbra
problems to the Magic Lantern project. Report them here.

> **Status: do not install Penumbra on a camera yet.**
> Every change has been checked only in emulation (QEMU) and with host-side tests. Nothing has run on
> real hardware. See [Status](#status) and [docs/LIMITS.md](docs/LIMITS.md).

## Who it is for

Photographers who use an EOS M with Magic Lantern for still photography (focus peaking, zebras,
histogram, intervalometer, bracketing, ETTR, dual ISO) and want the battery to last longer. Penumbra does
not support video work: raw video, audio tools and FPS overrides are removed from the default build.

The EOS M has no mirror and is always in LiveView while shooting. The sensor and the display use most of
the battery, and no software running next to Canon's firmware can change that much. Penumbra cuts what
Magic Lantern adds on top: background tasks that wake the CPU many times a second, overlays that are
redrawn when nothing has changed, and a permanent block on Canon's low-power idle modes.
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) explains the approach.

## Status

| Area | State |
|---|---|
| Photo build profile (EOS M) | Done, on `main` |
| Faster overlay pixel kernels (zebras, peaking, histogram) | Done, on `main`, bit-exact against the original code in host tests |
| Low-risk wakeup reductions | Done, on `main` |
| Counted power-save holds (stop blocking Canon's low-power idle) | Done, on `main` |
| Lens-info poll fix (stops 3-4 Canon power-save toggles/s at idle) | Done, on `main` |
| Card I/O safe across deep sleep (each file call holds power save) | Done, on `main` |
| Deep sleep in the emulator (1100D) | Done: Canon alone and Penumbra enter Canon's deep sleep with the display off; Magic Lantern never does |
| Event-driven core | Designed, not started |
| Canon 1100D | Used as the emulation test body: the whole chain runs there first |
| Canon EOS M emulation | Waiting for a ROM dump. CI skips EOS M scenarios until one is available |
| Real hardware | **Not tested** |
| Canon 4000D port | In progress, emulation only: 161 of 167 Canon stubs found and checked against the ROM; photo build (intervalometer, bracketing, bulb timer, file manager, Lua, counted power-save holds) boots and its menus work in emulation; no LiveView yet ([docs/PORT-4000D.md](docs/PORT-4000D.md)) |

Measured and estimated numbers so far are in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#4-where-we-are).
Penumbra makes no claims about battery life (percentages, shots per charge) until it has been measured on a
real camera.

## Features compared with Magic Lantern

The default `photo` profile keeps the stills tools and removes the video, audio and developer features.
The `full` profile builds the same feature set as upstream Magic Lantern for the EOS M. CI uses it as the
comparison baseline.

**Kept (photo profile):** zebras, focus peaking, histogram, magic zoom, spotmeter, cropmarks, intervalometer,
bracketing / HDR, bulb timer, auto ETTR, dual ISO, silent pictures, the ML menu and its photo settings, and
Lua scripting as an optional module. Shipped modules: `adv_int`, `autoexpo`, `deflick`, `dual_iso`, `ettr`,
`file_man`, `img_name`, `lua`, `pic_view`, `sd_clock`, `silent`.

**Removed (photo profile):** raw/MLV video (`mlv_lite`, `mlv_play`, `mlv_snd`, `crop_rec`), FPS override and
ramping, bitrate control, HDR video, movie logging/restart/record-key tweaks, gradual exposure, vignetting
correction, audio meters, audio remote shot, beeps, and the developer/toy modules (`bench`, `selftest`,
`edmac`, `arkanoid`). The full list is in [docs/BUILD-PROFILES.md](docs/BUILD-PROFILES.md).

**Not available on the EOS M (same as upstream):** trap focus.

**Behaviour changes** that come from the power work (for example, settings changed outside the menu are
saved later, and a feature that runs from timers must take a "hold") are listed in
[docs/LIMITS.md](docs/LIMITS.md#behaviour-changes-compared-with-magic-lantern).

## Building

You need the Arm GNU Toolchain 12.2.rel1 (`arm-none-eabi-gcc`) on `PATH`, plus `make`, `python3`
(docutils, pycparser) and `zip`. `ci/Containerfile` describes the exact environment CI uses.

```
cd platform/EOSM.202
make clean && make -j4 FATAL_WARNINGS=y                   # photo profile (default)
make clean && make -j4 FATAL_WARNINGS=y ML_PROFILE=full   # upstream-equivalent feature set
```

The result is `platform/EOSM.202/build/magiclantern.zip`. Always run `make clean` when you switch profiles.
The 1100D test platform builds the same way from `platform/1100D.105`.

To build exactly as CI does (needs podman):

```
podman build -t penumbra-ci -f ci/Containerfile ci/
podman run --rm -v "$PWD":/work -w /work penumbra-ci ci/build.sh
```

Profiles and build switches: [docs/BUILD-PROFILES.md](docs/BUILD-PROFILES.md). CI details: [docs/CI.md](docs/CI.md).

## How changes are verified

Hardware is the last step. Before that, every change has to pass:

1. **Build checks.** The EOS M builds with warnings as errors, and the report lists binary size and task
   count against a baseline. cppcheck runs as a report.
2. **Host tests.** The overlay kernels are compared with the original Magic Lantern loops on synthetic frames
   and must match bit for bit. ARM926 instruction counts are measured with the camera compiler. The power
   branches add unit tests for their own logic, such as the power-save hold bookkeeping.
3. **Emulation.** [qemu-eos](https://github.com/reticulatedpines/qemu-eos) boots the Canon firmware with
   and without Penumbra, headless, in CI. A small QEMU plugin counts CPU wakeups, interrupts and DryOS task
   switches. Scripted scenarios open menus and take screenshots, and the results are compared with the
   Canon-only baseline and with the upstream-equivalent build. See [docs/EMULATION.md](docs/EMULATION.md).

QEMU has no power model, so emulation numbers are relative proxies (wakeups, task switches, how often the
power-save counter changes). They are not battery measurements.

Canon firmware dumps are never stored in this repository. CI fetches the maintainer's own dumps from a private
package registry, using credentials stored as CI secrets. Without them, the emulation scenarios are skipped.

## Documentation

All documents are listed in [docs/README.md](docs/README.md). The main ones:

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): today's Magic Lantern architecture, Penumbra's event-driven
  target, and progress so far
- [docs/LIMITS.md](docs/LIMITS.md): known problems, emulation limits, behaviour changes
- [docs/PLAN.md](docs/PLAN.md): project phases
- [docs/BUILD-PROFILES.md](docs/BUILD-PROFILES.md), [docs/CI.md](docs/CI.md), [docs/EMULATION.md](docs/EMULATION.md)

The upstream Magic Lantern documentation (`doc/`, `developer_guide/`) is kept as it is. For Magic Lantern
itself, see <https://www.magiclantern.fm/> and the
[forum](https://www.magiclantern.fm/forum/).

## Credits and license

Penumbra is based on Magic Lantern by the Magic Lantern developers and contributors:
<https://github.com/reticulatedpines/magiclantern_simplified>. Magic Lantern runs alongside Canon's own
firmware: it is loaded from the memory card at boot and does not replace the Canon firmware.
The emulator is the Magic Lantern team's [qemu-eos](https://github.com/reticulatedpines/qemu-eos).

Penumbra is released under the **GNU General Public License, version 2**, like Magic Lantern. See
[COPYING](COPYING). All existing copyright notices are kept. Penumbra's changes are under the same license.

Penumbra is not affiliated with or endorsed by the Magic Lantern project or by Canon Inc. "Canon" and "EOS"
are trademarks of Canon Inc. This repository contains no Canon code. Firmware addresses and descriptions of
firmware behaviour in `docs/firmware/` come from analysis of the maintainer's own camera dumps and are written
in our own words.

Running unofficial software on a camera carries risk. Use it at your own risk; the software comes with no
warranty (see COPYING).
