# Canon update file EOSM1202.FIR (fw 2.0.2)

Result: **not usable for static RE without a ROM dump** — payload is AES-encrypted ("2010 keys" family,
same as 550D..6D), key not public. ROM0/ROM1 dumps from the camera remain the only route (phase 1).

- Source (Canon CDN): `https://gdlp01.c-wss.com/gds/9/0400000869/01/eosm-v202-win.zip`
  - zip SHA-256 `18d455d5189af70f8d6065a05a2ceb10f006d74b2dfcb46c9f970215ba962080`
  - `EOSM1202.FIR` 17,743,884 B, SHA-256 `6cd02eda42cdb9ae77e756e0768af6d8273b3e7901e9dcfd439bd44feb521688`
- Header (matches `tools/indy/fir_tool2.py` / ML wiki "Firmware file"): model `0x80000331`, version "2.0.2",
  byte-sum checksum @0x20 valid, HMAC-SHA1 @0x68 (updater) and @0x88 (firmware).
- Updater @0x120, len 0x1EC9D0; firmware section @0x1ECAF0, len 0xEFF51C. Both payloads have 8.0 bits/byte entropy.
- The 512/513-byte XOR tables in `fir_tool2.py` (40D..500D era) do not apply.
- Even if decrypted it would lack the bootloader, ROM0 and serial flash, so it could not replace dumps for QEMU.

Refs: ML wiki Fir_Security (Wayback); Check Point 2019 80D write-up; hack.lu 2025 DIGIC 8 slides.
