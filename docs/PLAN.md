# Penumbra — Plan

Goal: Penumbra, a photo-focused, battery-efficient Magic Lantern build for the original Canon EOS M (fw 2.0.2),
derived from upstream `reticulatedpines/magiclantern_simplified` (`dev`, GPL-2.0), verified in emulation
before any hardware run.

## Facts that shape the plan
- Upstream is active (last commit 2026-10-04) and still maintains `platform/EOSM.202`.
- Upstream runs ~30 background tasks and has ~455 `msleep()` calls: polling loops are the main
  suspected source of battery drain (CPU never idles). The ML FAQ notes that ML disables CPU powersave in some modes.
- The EOS M is always in LiveView (no mirror), so the display and LV pipeline dominate power. ML must never keep
  them alive longer than Canon would.
- `qemu-eos` (v4.2.1 branch) knows the EOSM model. It needs **ROM0/ROM1/SFDATA dumps from the maintainer's own camera**.
  There is no LiveView image in emulation, so the overlay algorithms are tested on the host with recorded/synthetic frames.
- QEMU has no power model. The proxy metric is **wakeups/s, context switches/s and timer IRQs/s at idle**, compared
  against the upstream baseline.
- The Canon firmware itself (ROM dumps) is never committed to GitHub.

## Phases
| # | Phase | Output | Exit criterion |
|---|---|---|---|
| 0 | **Infra** | Upstream imported with full history; self-hosted Forgejo mirror + `.forgejo/workflows` on a podman runner; pinned build container (arm-none-eabi + python3); dual push GitHub+Forgejo | CI builds unmodified `EOSM.202` → `magiclantern.zip` |
| 1 | **Firmware acquisition** *(maintainer, read-only)* | ROM0/ROM1/SFDATA dumped with existing ML build; stored only in a private package registry | MD5 recorded in `docs/firmware/` |
| 2 | **Firmware RE** (Ghidra) | Ghidra project; verified `stubs.S` for 2.0.2; map of DryOS scheduler/idle, power-off timers, LV/display power, prop/event system | `docs/firmware/*.md` with addresses + evidence |
| 3 | **Emulation harness** | qemu-eos built in container; headless boot → Canon GUI → ML menu; key-injection + screenshot tests for EOSM; QEMU plugin counting wakeups/IRQs/task switches | CI job: boot test + idle-wakeup baseline of upstream |
| 3b | **QEMU performance** | Post-boot snapshots (savevm/loadvm) so tests skip the boot; near-zero-cost wakeup plugin (translation-time, inline counters); quiet device logging, no display/VNC unless needed; optimized qemu build (-O2/LTO, arm-softmmu only); TCG tuning; parallel test runs | Measured on 1100D vs baseline: boot time, time per test, emulated MIPS; identical screenshots/logs |
| 4 | **Photo profile** | Feature set trimmed: keep zebras, focus peaking, histogram, magic zoom, intervalometer, bracketing/HDR, bulb timer, auto-ETTR, dual ISO, (trap focus if feasible), optional Lua; drop video/RAW/MLV/crop_rec/FPS/bitrate/audio/games/bench | Smaller binary + fewer tasks, all tests green |
| 5 | **Power work** | Polling → event-driven (prop handlers, semaphores, msg queues); task consolidation; overlays run only while LV is visible and something changed; never block Canon auto-power-off/LV powersave | Idle wakeups ≪ baseline; sleep/power-off paths covered by emulation tests |
| 5b | **Event-driven core** | One ML core task blocked on a single message queue (Canon events, prop changes, LV vsync, one-shot timers) replaces the polling tasks one at a time (strangler pattern); features register handlers/timers only while enabled ("cold" when off); Lua kept as optional module, never in hot paths | Idle wakeups → ~0 in LiveView; each migrated task verified in emulation before the next. Designed after phase 2 RE of Canon's event/power model |
| 6 | **Overlay performance** | Peaking/zebra/histogram rewritten for ARMv5 (word-wide ops, decimation, frame skipping, LUTs); host unit + benchmark tests on YUV422 frames | Bit-exact vs reference, measurable cycle reduction |
| 7 | **Stability** | Known EOSM bugs triaged/fixed; long-run soak in emulation | Soak test green |
| 8 | **Hardware** *(last, by the maintainer)* | Install guide, rollback procedure, A/B battery protocol (shots per charge, LV minutes) | Maintainer sign-off |
| 9 | **Port 4000D** (after the chain is proven on 1100D) | See docs/PORT-4000D.md: QEMU model, stub RE from 1300D base, minimal boot, platform dir, photo features | ML menu + photo features green in emulation; hardware last |

## Design decisions
See also `docs/ARCHITECTURE.md` (event-driven target) and `docs/LIMITS.md`.
- Stay in C (no rewrite in Rust/other): power cost comes from the polling architecture, not the language; ML's value is 13 years of reverse-engineered camera knowledge. Hand-written ARMv5TE assembly only for proven hot pixel loops, guarded by host bit-exact tests.

## CI pipeline (self-hosted Forgejo, per push)
build (size + task-count report) → static analysis (cppcheck, -Wall -Werror on new code) → host unit tests
(overlay algorithms) → qemu boot + menu tests → idle-wakeup regression check vs baseline.

## Risks
- The camera may run fw **2.0.3**: ML only supports 2.0.2, so the official Canon firmware would need a downgrade (owner's call).
- EOSM emulation depth is unverified (GUI likely, LV/touch unlikely). Mitigation: host-side algorithm tests + GDB/log-based checks.
- Power numbers in QEMU are relative proxies; the real confirmation is phase 8.
- Divergence from upstream: changes are kept behind the photo profile or build switches where possible, so
  upstream work can still be merged; Penumbra's changes are not submitted upstream as-is.
