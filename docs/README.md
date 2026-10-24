# Penumbra documentation

Start with the [top-level README](../README.md). Penumbra is based on Magic Lantern. The upstream Magic Lantern
documentation is kept unchanged in `doc/` (user guide sources) and `developer_guide/`.

Some documents currently live only on branches that are not merged yet. Read them with
`git show <branch>:<path>`, or check out the branch.

## On `main`

| Document | What it covers |
|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | Magic Lantern's polling design today, Penumbra's event-driven target (diagrams), progress, and the numbers measured so far |
| [LIMITS.md](LIMITS.md) | Known problems, emulation limits, fundamental limits, and behaviour changes compared with Magic Lantern |
| [PLAN.md](PLAN.md) | Project phases, exit criteria, design decisions and risks |
| [BUILD-PROFILES.md](BUILD-PROFILES.md) | The `photo` and `full` build profiles for the EOS M: what each one contains and how to build it |
| [CI.md](CI.md) | The CI pipeline: build, host tests, cppcheck, artifacts, size baseline |
| [EMULATION.md](EMULATION.md) | The QEMU harness: scenarios, the wakeup plugin, metrics and their limits, 1100D findings, ROM handling |
| [PORT-4000D.md](PORT-4000D.md) | Plan for the Canon 4000D port, based on the 1300D platform |
| [analysis/power-audit.md](analysis/power-audit.md) | Audit of upstream ML on the EOS M: tasks and wakeup rates, power-save blockers, overlay hot paths, ranked fixes |
| [analysis/overlay-kernels.md](analysis/overlay-kernels.md) | Zebra, peaking and histogram kernels: bit-exact equivalence tests and ARM926 instruction counts |
| [firmware/1100D-powersave.md](firmware/1100D-powersave.md) | Canon 1100D `DisablePowerSave` / `EnablePowerSave`, the PowerMgr idle modes and self-refresh sleep (addresses and our own description) |
| [firmware/4000D-powersave.md](firmware/4000D-powersave.md) | Canon 4000D PowerMgr idle loop and `selfRefresh` (WFI with IRQs masked): addresses, registers, differences from the 1100D |
| [firmware/fir-update-file.md](firmware/fir-update-file.md) | Why the EOS M 2.0.2 update file cannot replace a ROM dump (encrypted payload) |
| [firmware/rom-md5.txt](firmware/rom-md5.txt) | Checksums of the maintainer's ROM dumps, used by CI to verify downloads (the dumps themselves are never in the repository) |

## On unmerged branches

| Document | Branch | What it covers |
|---|---|---|
| `docs/analysis/power-changes.md` | `power/low-risk` (extended on the later power branches) | The low-risk wakeup reductions, one section per change: expected gain, QEMU recipe, risks |
| `docs/analysis/powersave-hold.md` | `power/powersave-hold` (extended on `power/lens-poll` and `power/sd-safe`) | The counted power-save hold API, the list of holders, risks, first QEMU results, the lens-info poll fix and its measurements |
| `docs/EMULATION.md` (newer version) | `emulation-image` | The emulation job pulls a prebuilt, pinned QEMU image instead of building it, and runs scenarios in parallel |
