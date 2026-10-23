# Overlay kernels: equivalence and speed

The per-pixel loops of the LiveView overlays (software zebras, dot-mode focus peaking, YUV histogram) live in
`src/overlay_kernels.c/.h`, free of camera dependencies. `src/zebra.c` keeps buffer selection, colours, dirty-pixel
tracking, thresholds and scheduling. False colour, waveform/vectorscope and the hardware zebra grid keep their
original loops.

## Equivalence
`make -C tests/host` compares every kernel against a verbatim copy of the original loops (`tests/host/reference/`)
on six synthetic UYVY frame types (gradients, edges, seeded noise, clipped highlights…), the LCD, HDMI 1080 and RCA
layouts, a range of thresholds and overlay states, and both colour-conversion tables: 5 616 checks, all bit-exact.
Mutation check: 14 deliberate faults were injected into the new kernels; every behavioural one was caught.

## Speed
ARM926 instructions per frame, kernels built with the camera compiler and flags (`-Os -march=armv5te`), executed on
an emulated ARM926 (`make -C tests/host armcount`, needs the Arm toolchain and the `unicorn` Python package).
Output was bit-identical in this build too.

| Kernel | Original | New | Speed-up |
|---|---:|---:|---:|
| Peaking, default filter | 1.45 M | 0.76 M | 1.90× |
| Peaking, other two filters | 1.99 M | 1.55 M | 1.29× |
| Zebras, luma | 2.39 M | 1.49 M | 1.60× |
| Zebras, RGB | 5.32 M | 2.86 M | 1.86× |
| Histogram, luma | 3.49 M | 0.98 M | 3.56× |
| Histogram, RGB (EOS M default) | 7.29 M | 4.65 M | 1.57× |

Host timings (`make -C tests/host bench`) range 1.1×–3.6× and are indicative only.

## Limits
- Instruction counts ignore caches, memory wait states and pipeline stalls; real gains on the camera may differ.
- Zebras skip overlay writes when the pixel already has the right value: same result, different memory traffic.
- On ARMv5TE, byte loads were kept instead of 32-bit word unpacking (unpacking cost more instructions).
- The histogram relies on the four arrays of `struct Histogram` being contiguous (compile-time assert); RGB zebras
  assume the colour tables stay within ±768 (they are about ±240).
- Cost: about +2.5 KB of code.
