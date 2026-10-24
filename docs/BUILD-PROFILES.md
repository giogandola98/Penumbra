# Build profiles (EOS M 2.0.2, 4000D 1.1.0)

`platform/EOSM.202` can be built in two profiles, selected with `ML_PROFILE`:

| Profile | Command | Meaning |
|---------|---------|---------|
| `photo` (default) | `make` | Photo-only build; video, audio and dev features removed. |
| `full` | `make ML_PROFILE=full` | Upstream-equivalent build (identical autoexec.bin size to the pre-profile baseline). |

Toolchain: Arm GNU Toolchain 12.2.rel1 (`arm-none-eabi-gcc`) on `PATH`, as in `ci/Containerfile`.

```
cd platform/EOSM.202 && make clean && make -j4 FATAL_WARNINGS=y            # photo
cd platform/EOSM.202 && make clean && make -j4 FATAL_WARNINGS=y ML_PROFILE=full
```

Always `make clean` when switching profile (the profile is a compiler flag, `-DCONFIG_PROFILE_PHOTO` / `-DCONFIG_PROFILE_FULL`).

## What the profile controls

* `platform/EOSM.202/Makefile`: sets `ML_PROFILE`, the compiler flag, and `MODULES_INCLUDED`.
* `platform/EOSM.202/features-photo.h`: `#undef`s video/audio/dev `FEATURE_*` flags. It is included at the end of
  `features.h` only when `CONFIG_PROFILE_PHOTO` is set, so `features.h` stays close to upstream.
* `platform/EOSM.202/modules-photo.included`: modules packed into `magiclantern.zip` for the photo profile
  (adv_int, autoexpo, deflick, dual_iso, ettr, file_man, img_name, lua, pic_view, sd_clock, silent).
  `modules.included` is unchanged and used by `full`.
* `platform/Makefile`: two generic hooks, `MODULES_INCLUDED` (default `modules.included`) and
  `ML_SRC_OBJS_EXCLUDE` (core objects a profile may drop; empty for now).
* Small `CONFIG_PROFILE_PHOTO` / `FEATURE_FPS_OVERRIDE` guards in `src/audio-common.c`, `src/audio-ak.c`,
  `src/fps-engio.c` (do not create the audio polling task, the empty Audio/Movie FPS menus, and fix a
  prototype that only compiled when FPS override was on).

* `POWERSAVE_HOLD` (make variable, `platform/Makefile`): `y` adds `-DCONFIG_POWERSAVE_HOLD`, which replaces ML's
  permanent boot-time `DisablePowerSave` with counted holds taken only while a feature needs timers
  (`docs/analysis/powersave-hold.md`). The photo profile sets `POWERSAVE_HOLD ?= y`; `full` and other cameras keep
  upstream behaviour unless built with `make POWERSAVE_HOLD=y` (e.g. the 1100D QEMU validation build).
  `POWERSAVE_HOLD=n` opts the photo profile out. `make clean` when changing it.

Modules are camera- and profile-independent; all of them are still compiled, the profile only chooses which are
shipped in the zip.

## Not removed from the link (and why)

`movtweaks.o`, `fps-engio.o`, `beep.o`, `bitrate.o`, `audio-ak.o`, `crop-mode-hack.o` stay linked. They are
feature-gated and mostly compile to nothing with the photo feature set (a few KB total), but also hold shared code:
screen layout, `bv_iso`/expo override, shutter-blanking helpers used by `lens.c`/`tweaks.c`, `sound_recording_enabled*`
used by `menu.c`/`bmp.c`, `is_mvr_buffer_almost_full` used by `zebra.c`. Dropping them needs those pieces split out first.

Trap focus stays disabled as in upstream EOSM (`#undef FEATURE_TRAP_FOCUS`). `silent` is kept (silent pictures are a
photo feature); `lua` is shipped as an optional module.

## 4000D 1.1.0

`platform/4000D.110` has the same switch, but there is no upstream feature set for this camera, so the profiles
start from the bring-up set (`features.h`: debug menus only, no LiveView, movie, raw, FPS or audio):

| Profile | Command | Meaning |
|---------|---------|---------|
| `photo` (default) | `make` | Bring-up set + `features-photo.h`, `POWERSAVE_HOLD ?= y`, modules from `modules-photo.included` |
| `full` | `make ML_PROFILE=full` | Bring-up set only: the comparison baseline in emulation (`ml-full`), permanent `DisablePowerSave` |

Photo features are added one at a time, each built with `FATAL_WARNINGS=y` and booted in emulation
(`docs/PORT-4000D.md`, step 5):

| Feature | How | Emulation check |
|---|---|---|
| Intervalometer | `FEATURE_INTERVALOMETER` | Shoot tab entry (`menu_tour01_right.png`); `ml-photo-ivl`: auto-started by a config preset, its hold taken and held, Canon counter 1 in every sample |
| HDR bracketing, bulb timer | `FEATURE_HDR_BRACKETING`, `FEATURE_BULB_TIMER`; Canon property writes on, limited to `property_whitelist.h` (`CONFIG_PROP_WRITE_WHITELIST`) | Shoot tab entries; no `PROP_LEN` asserts in the log. Not exercised (no picture taken in emulation) |
| File manager, image names | modules `file_man`, `img_name` | loaded at boot (Modules tab: OK) |
| SD clock | module `sd_clock`, shipped but off (no `.en` file) | listed as OFF |
| Lua | module `lua` (optional) | loaded at boot |

`CONFIG_PROP_WRITE_WHITELIST` (`src/property.c`) reuses the per-property write whitelist of the DIGIC 6/7/8 ports:
`prop_request_change` only reaches Canon for the properties in the platform's `property_whitelist.h` (4000D:
shutter, aperture, ISO, exposure compensation, drive mode, remote half/full shutter); every other write is
dropped as if `CONFIG_PROP_REQUEST_CHANGE` were off.
