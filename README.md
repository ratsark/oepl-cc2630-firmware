# OpenEPaperLink Firmware for CC2630 (TG-GR6000N)

Custom open-source OEPL firmware for the Solum TG-GR6000N 6.0" BWR e-paper tag.

## Hardware

- **Chip**: Texas Instruments CC2630F128 (ARM Cortex-M3, 48MHz, 128KB Flash, 20KB RAM)
- **Radio**: 2.4 GHz IEEE 802.15.4 (OEPL protocol)
- **Display**: 6.0" UC8159 e-paper controller, 600x448 BWR (black/white/red)
- **Model**: Solum TG-GR6000N (hwType 0x35)
- **MAC**: 00:12:4B:00:18:18:80:B0

## Current Status

**Fully working end-to-end.** The tag receives images from the OEPL AP and displays them.

- [x] RF core boot and IEEE 802.15.4 radio
- [x] AP channel scanning (6 channels: 11, 15, 20, 25, 26, 27)
- [x] OEPL checkin protocol (AvailDataReq/AvailDataInfo)
- [x] Block transfer with cumulative part tracking (42 parts/block), 8-entry RX ring, burst idle-timeout
- [x] OTA firmware update via the AP (`tools/ap.py ota`)
- [x] Crash/hang recovery: HardFault, RF-core hang and watchdog reset the tag and report `wakeupReason=0xFE` (fault PC on the splash and in the AP DB)
- [x] Panel powered off and deep-slept after every refresh
- [x] UC8159 display driver with OTP waveform loading
- [x] BWR (black/white/red) image display - 1bpp per layer, 17 blocks
- [x] Compressed images (`DATATYPE_IMG_ZLIB`) decoded on the tag — a picture is
      ~2 KB on the air instead of ~67 KB, one block instead of 17
- [x] NFC: writes the tag's identity to the on-board NTAG chip and accepts URLs
      pushed from the AP's "Set NFC URL" content mode
- [x] Check-in retried up to 3x within one wake — 68% → 88% of wakes complete
      at a marginal spot
- [x] Standby between check-ins, **9.1 µA measured**
- [x] CCFG backdoor enabled (DIO11 LOW enters bootloader)
- [x] SEGGER RTT debug output (512-byte buffer)
- [x] UART TX debug mirror on DIO3 at 115200 baud (off by default)

**Firmware**: v0.33 — 27 KB flash, 20 KB RAM including the stack (see `PLAN.md`
for history, `DEVELOPMENT.md` for the test loop)

**Battery**: ≈0.83 mAh/day for the reference weather tag — **check-in every
15 minutes, image update every 2 hours** (12 refreshes a day), which works out
to roughly **6 years on 4x CR2450**, far enough out that the cells' own
self-discharge matters as much as the tag does. Updates are 71% of that budget
and sleep 27%, so the number moves with how often you refresh, not with how
often you check in. Measured, not estimated; the per-item figures and the
per-pin sleep table are in `DEVELOPMENT.md`.

## Project Structure

```
oepl-cc2630-firmware/
├── Makefile              Build system
├── firmware/             Source code
│   ├── main.c            Entry point, checkin loop, download+display
│   ├── startup_cc2630.c  Reset handler, vector table, HardFault handler
│   ├── ccfg.c            CC2630 customer configuration
│   ├── cc2630f128.lds    Linker script
│   ├── rtt.c/h           SEGGER RTT + UART TX debug output
│   ├── oepl_radio_cc2630.c/h    OEPL radio protocol (scan, checkin, blocks)
│   ├── oepl_rf_cc2630.c/h       Low-level RF core driver
│   ├── oepl_hw_abstraction_cc2630.c/h  GPIO, SPI, delays
│   └── drivers/
│       └── oepl_display_driver_uc8159_600x448.c/h  UC8159 display driver
├── binaries/             Pre-built firmware binary
├── docs/                 Analysis and development documentation
├── reference/            Notes on the vendor dumps (binaries not distributed)
└── tools/                Utility scripts
    ├── ap.py             AP helper: OTA push, image push, poll tag state, decode fault/diag reports
    ├── welcome_cards.py  A one-off character card for each tag that joins an AP
    ├── deep_thoughts_display.py  Random Jack Handey quote on a tag, as an HA automation
    ├── flash_s2.py       UART bootloader flash through an OEPL S2-mini Tag-Flasher
    ├── ap_log.py         Stream the AP's live log (block requests, xfer complete, timeouts)
    ├── weather_display.py  Home Assistant weather layout: preview, push, install automation
    ├── flash.sh          UART bootloader flash script
    ├── xflash.sh/.ps1    XDS110 (UniFlash) flash script, uses cc2630_xds110.ccxml
    ├── dl_pin.sh         D/L pin (GPIO17) control
    └── start_fw.jlink    JLink firmware launch script
```

## Access points

[docs/SPAGHETTI_AP.md](docs/SPAGHETTI_AP.md): wiring and flashing a hand-wired
ESP32-S3 + ESP32-C6 AP from this Mac, with the pitfalls met building the second one.

## Build Requirements

### ARM Toolchain

```bash
sudo apt install gcc-arm-none-eabi gdb-multiarch libnewlib-arm-none-eabi
```

### TI Driverlib

The CC26x0 driverlib must be built at `~/Code/ti/cc26x0/driverlib/`.
See `docs/BUILD_STATUS.md` for details.

## Building

```bash
make                    # Build firmware
make size               # Show memory usage
make clean              # Clean build artifacts
```

## Programming

### Via J-Link (cJTAG) - Recommended

Requires SEGGER J-Link connected via cJTAG (2-wire).

```bash
# Start GDB server
JLinkGDBServer -device CC2630F128 -if cJTAG -speed 1000 \
  -port 2331 -RTTTelnetPort 19021 -notimeout &

# Flash and start firmware
gdb-multiarch -batch -nx \
  -ex "file build/Tag_FW_CC2630_TG-GR6000N.elf" \
  -ex "target remote :2331" \
  -ex "monitor halt" \
  -ex "monitor flash erase" \
  -ex "load" \
  -ex "set \$pc = 0x000000bc" \
  -ex "set \$sp = 0x20005000" \
  -ex "monitor go" \
  -ex "disconnect"
```

**Important**: Always `monitor flash erase` before `load` to avoid stale flash corruption.

**Note**: `monitor reset` does NOT work reliably on CC2630. Must set PC/SP manually.

### Via UART Bootloader (cc2538-bsl)

Requires CCFG backdoor enabled (byte 48 = 0xC5) in the currently-flashed firmware.
The D/L pin (DIO11) is controlled by Raspberry Pi GPIO17.

```bash
./tools/flash.sh binaries/Tag_FW_CC2630_TG-GR6000N.bin /dev/ttyUSB0
```

### Via TI XDS110 (LaunchPad) with UniFlash

Any TI LaunchPad's on-board XDS110 works as a cJTAG programmer, no J-Link needed.
Requires [TI UniFlash](https://www.ti.com/tool/UNIFLASH) (free) installed in its
default location. Wiring, the UniFlash target config and troubleshooting are in
[docs/FLASHING_XDS110.md](docs/FLASHING_XDS110.md).

```bash
./tools/xflash.sh binaries/Tag_FW_CC2630_TG-GR6000N.bin    # tools\xflash.ps1 on Windows
```

## Debugging

### RTT (with J-Link)

```bash
# Read RTT output (requires JLinkGDBServer running)
timeout 30 bash -c 'exec 3<>/dev/tcp/localhost/19021; cat <&3'

# Or use JLinkRTTClient
JLinkRTTClient
```

### UART

Debug output also goes to UART0 TX (DIO3) at 115200 baud, compatible with
the FTDI adapter used for cc2538-bsl flashing.

## Pin Assignments

| DIO | Function | Notes |
|-----|----------|-------|
| 2   | UART RX  | cc2538-bsl / debug |
| 3   | UART TX  | cc2538-bsl / debug |
| 5   | EPD Power | Enable (tentative) |
| 8   | SPI MISO | |
| 9   | SPI MOSI | |
| 10  | SPI CLK  | |
| 11  | Flash CS / D/L | Dual-use: SPI CS for flash, bootloader entry |
| 12  | DIR      | LOW = write |
| 13  | BUSY     | Input, HIGH = not busy |
| 14  | RST      | Active LOW |
| 15  | DC       | Data/Command |
| 18  | BS1      | LOW = 4-wire SPI |
| 20  | EPD CS   | GPIO output (not SSI0 FSS) |

## Known Issues

- **A marginal link costs check-ins, but not for the reason it looks like.**
  Measured with the tag shielded to ≈−74 dBm: 28% of check-ins failed because
  the AP never heard the *request*, against 4% where the tag missed the reply.
  So it is uplink, not receive sensitivity — an RF_CFG sweep (CPE patch and
  override combinations, 4 configs x 2 rounds) found no significant difference.
  Retrying the request up to 3x within one wake takes 68% of wakes to 88%.
  See DEVELOPMENT.md, "Radio: what limits a marginal link".
- **A compressed image larger than 20 KB will not display.** Images are staged
  in flash sectors 16–20 before decoding; anything bigger is refused and
  retried. The weather layout is ~6 KB and a test card ~2.2 KB, so there is
  room, but a heavily dithered photograph could exceed it.
- **Upgrade from v0.32 or earlier if your AP is not on channel 11.** Until
  v0.33 the tag reported its *scan index* (0-5) as `currentChannel` instead of
  the IEEE channel number. The AP drops a check-in from a MAC it does not
  already know when `currentChannel > 0` and it disagrees with the AP's own
  channel, so a **brand-new tag never registered** on any AP not on channel 11.
  Tags already in the AP's database were exempt, which hid it, and index 0 —
  channel 11 — skips the check entirely. Found and fixed by @knyghtryda (#11).
- **OEPL channel 27 is unusable on this chip** — `CMD_IEEE_RX` takes 11–26
  only, and channel 27 wedged the radio core until the tag reset (fixed in
  v0.22 by skipping it; the AP must not be set to channel 27).
- **The reported firmware version selects the AP's image format.** At 39 or
  above the AP sends zlib-compressed images (which this firmware decodes);
  below 39 it sends raw. See DEVELOPMENT.md before changing the version.
- **NFC is supported** (v0.30): the board carries a passive NTAG I2C chip with a
  multi-turn coil on the back of the PCB. The tag writes its identity there at boot and
  accepts URLs pushed from the AP's "Set NFC URL" content mode. A tap works even on a flat
  or bricked tag, since the chip is powered by the reader.
- **A third antenna on the board is still unidentified** — a ~17.3 mm trace in the top-left
  corner with its own 8-pad IC (marked `S92` / `742`), separate from both the 2.4 GHz
  antenna and the NFC coil. See DEVELOPMENT.md, "The three antennas".
- **UART debug mirror** off by default (`-DRTT_UART`); it never produced
  output on this board and kept the serial domain powered. Use RTT.

Fixed since the first release, and no longer issues: sleep current (≈2 mA →
59 µA → **9.1 µA**), image transfer time (~100 s → ~45 s, and 1–2 blocks
instead of 17 now that images are compressed), the radio core hanging on
channel 27, and DIO13/BUSY — the panel does drive BUSY and refreshes now end
on it (~7.7 s) rather than running to a timeout.

## Contributors

This firmware is better than it would have been because two people took the
trouble to review it carefully and say what was wrong.

**[@spectrumjade](https://github.com/spectrumjade)** (Justin Gerace) — PRs #5,
#6 and #7, each with a J-Link diagnosis behind it rather than a guess. The
deep-sleep path was genuinely broken and the missing
`SysCtrlSetRechargeBeforePowerDown()` was real; the block-transfer write-up
correctly identified the `IEEE_SUSPENDED` status blip between TX and RX, and
that the early bail on empty responses was too impatient; and the hang was
located inside `RFCDoorbellSendTo`. The branches were superseded by parallel
work on master before they could be merged, which is a poor reward for being
right — the findings shaped what landed.

**[@PeitzGreene](https://github.com/PeitzGreene)** — an 8-dimension review of
the whole tree with every claim put to a separate adversarial verifier: 73
raised, 8 refuted, 65 documented with reproduction and a suggested fix. It
caught things testing does not: flash readbacks passing because they went
through the VIMS cache, the OTA `dataVer` being committed before the apply, a
staged reset vector accepted without its Thumb bit, OEPL channel index 5 mapping
to an IEEE channel `CMD_IEEE_RX` rejects, and a HardFault handler that had been
reporting R12 as the fault PC.

Thanks also to everyone who opened an issue — several of them were the first
sign of a real bug.

## License

[GNU General Public License v3.0 or later](LICENSE).

You may use, study, modify and redistribute this firmware. If you distribute a
modified version — including flashed onto tags you hand to someone else — you
must make your source available under the same terms. That is the point: these
tags are e-waste being kept useful, and fixes should reach everyone who has one
rather than stopping at a private fork.

A note on relationship to upstream: the OpenEPaperLink project is licensed
CC BY-NC-SA 4.0, which is not compatible with the GPL in either direction. No
code is shared between the two — this is an independent implementation for a
chip OEPL does not target, and it speaks OEPL's wire protocol, which is not
itself a copyrightable thing. If OEPL ever wants any of this upstream, ask and
it can be relicensed for that purpose.

## Based On

- [OpenEPaperLink](https://github.com/OpenEPaperLink/OpenEPaperLink) project
- CC2630 OEPL alpha firmware (analysed locally; see `reference/README.md`)
- TG-GR6000N stock firmware (display init sequence extracted via Ghidra)
