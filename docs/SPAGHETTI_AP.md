# Building a "spaghetti" OpenEPaperLink AP (ESP32-S3 + ESP32-C6, hand-wired)

How the second AP was built on 2026-09-29, written down so the
next two go faster. The upstream guide is
[Beginners Guide for an easy to build Access Point](https://github.com/OpenEPaperLink/OpenEPaperLink/wiki/Beginners-Guide-for-an-easy-to-build-Access-Point-(aka-spaghetti-AP));
this page adds the exact pin labels, the esptool commands, and the things that
went wrong.

## Parts

- **ESP32-S3 devkit, N16R8** (16 MB flash, 8 MB PSRAM). Used: Hosyond 3-pack,
  ESP32-S3-WROOM-1 style, two USB-C ports labelled `USB` and `COM`.
  The firmware is the upstream **Yellow AP** build, which expects exactly this
  flash/PSRAM. The Yellow AP also drives a TFT and an LED on other pins; leaving
  those unconnected is fine. The devkit's RGB LED on GPIO48 matches the build.
- **ESP32-C6-DevKitC-1** (Espressif). Runs the C6 radio firmware; this is the
  802.15.4 side the tags talk to.
- 8 female-female jumper wires. A USB-C **data** cable (see Gotchas).

## Wiring

S3 GPIO number to the label printed on the C6 header:

| Purpose                     | S3 GPIO | C6 silkscreen |
|-----------------------------|---------|---------------|
| Data, S3 → C6               | 17      | 2             |
| Data, C6 → S3               | 18      | 3             |
| Flashing UART (S3 side)     | 19      | TX            |
| Flashing UART (S3 side)     | 20      | RX            |
| Boot-mode select            | 21      | 9             |
| Reset                       | 47      | RST           |
| Power                       | 3V3     | 3V3           |
| Ground                      | GND     | G             |

On the C6 header, `3V3` and `RST` are the top two pins on the left; `2` and `3`
are near the bottom of the left side; `TX`, `RX` and `9` are on the right, with
`TX`/`RX` just below the top `G`. `RST` is the chip's EN pin and `TX`/`RX` are
GPIO16/17 (UART0) -- they are just labelled by function.

The first two rows plus power and ground are all an AP needs to run. The other
four exist so the S3 can reflash the C6 from its web UI later.

Pin source: the upstream
[Access point pinouts](https://github.com/OpenEPaperLink/OpenEPaperLink/wiki/Access-point-pinouts)
table (Yellow AP column vs ESP32-C6 column) and `ESP32_AP-Flasher/platformio.ini`
env `ESP32_S3_16_8_YELLOW_AP` (`FLASHER_AP_TXD=17 RXD=18 DEBUG_TXD=19
DEBUG_RXD=20 DEBUG_PROG=21 AP_RESET=47`).

## Flashing from the Mac (no browser needed)

`~/Code/OpenEPaperLink/Tag_Flasher/flash_spaghetti.sh s3|c6` wraps the commands
below: finds the board, checks chip and flash size, flashes, verifies.

The web flasher at <https://install.openepaperlink.de> ("Yellow AP" for both
chips) works too, but esptool from the OEPL checkout's venv is quicker and
scriptable. Images, both full flash dumps written at offset 0:

```bash
cd ~/Code/OpenEPaperLink
curl -sLO https://install.openepaperlink.de/bin/current/ESP32_S3_16_8_YELLOW_AP_full.bin
curl -sLO https://install.openepaperlink.de/bin/current/OpenEPaperLink_esp32_C6.bin
```

**S3** (plug the `COM` port; it enumerates as a CH343, `/dev/cu.usbmodem…`):

```bash
venv/bin/esptool --port /dev/cu.usbmodem* flash-id          # expect 16MB, Embedded PSRAM 8MB
venv/bin/esptool --port /dev/cu.usbmodem* --baud 921600 --after hard-reset \
    write-flash 0x0 ESP32_S3_16_8_YELLOW_AP_full.bin
```

**C6** (plug its `UART` port; CP2102N, `/dev/cu.usbserial-…`). Flash it **before
wiring it to the S3**, or with the four signal wires pulled (see Gotchas):

```bash
venv/bin/esptool --port /dev/cu.usbserial-* --baud 921600 --after hard-reset \
    write-flash 0x0 OpenEPaperLink_esp32_C6.bin
```

If esptool reports "Wrong boot mode" or "No serial data received": hold `BOOT`,
tap `RST`, release `BOOT`, then rerun with `--before no-reset`.

## Bring-up

1. Wire per the table, power the S3 over USB (the C6 runs off the S3's 3V3).
2. Join the WiFi network `OpenEpaperLink`, open <http://192.168.4.1/setup>,
   enter the home WiFi. Mind the SSID spelling; a typo leaves it in setup mode.
3. Find it: hostname is `OpenEpaperLink-XXXX` (last two bytes of the S3's MAC,
   as esptool printed it), or sweep the subnet and look for the MAC in `arp -a`.
4. Check `http://<ip>/sysinfo`: `"ap_version"` must be non-zero (31 for C6
   firmware 0x1f). Zero means the S3 is not hearing the C6: check the 17↔2 and
   18↔3 wires are crossed as in the table.

Both APs here default to **channel 11**. A tag only re-scans when it loses its
AP, so tags move between APs when their batteries are pulled and they come up
nearer the other one. The Home Assistant integration and `tools/ap.py` only
know the original AP (set `OEPL_AP` to point tools at another one).


## Adding the 915 MHz radio (EByte E07-900M10 / CC1101)

Module pin to the C6's printed label; the C6 radio image already includes the CC1101 driver and
the Yellow AP build enables sub-GHz when the C6 reports the module.

| Module | C6 |
|--------|----|
| SI / MOSI | 1 |
| SO / MISO | 7 |
| SCLK | 0 |
| CSn | 4 |
| GDO0 | 5 |
| GDO2 | 6 |
| VCC | 3V3 |
| GND | G |

Antenna on before power. `get_ap_config` shows `hasSubGhz: 1` when it is detected; set
`subghzchannel` (203 = 915 MHz band, what the house AP uses) via `POST /save_apcfg`.

## Gotchas met on the way

- **The COM port needs an A-to-C path.** Its connector lacks the CC resistors, so a C-to-C cable
  gives it no power at all. A USB-C-to-A dongle plus an A-to-C cable works every time.
- **Power the C6 from 3V3, not 5V.** On the Hosyond S3 the 5 V header pin is not fed from the COM
  connector, so a 5V-to-5V link leaves the C6 dead: the S3 boots, reports no radio and sits in
  AP state 5 (failed). 3V3-to-3V3 works. The CC1101 module must also be on 3V3.
- **Wi-Fi settings reset:** press and release RST, then hold BOOT for six seconds. The AP wipes
  SSID/password/static IP and brings the `OpenEPaperLink` setup network back. Don't set a static
  address in 192.168.4.x: that is the setup portal's own subnet.
- **Charge-only USB-C cables.** The first cable powered the board but the Mac
  saw no USB device at all (not even an unknown one, so not a driver problem).
  If `system_profiler SPUSBDataType` shows empty buses, swap the cable.
- **Flashing the C6 over USB while wired to the S3 fails.** The C6's 3.3 V
  back-feeds the S3 through the power wire, the S3 boots and drives the C6's
  RST/9/TX/RX lines, and the handshake dies. Pull those four wires at the C6
  (leave 3V3, G, 2, 3), do the BOOT/RST dance, flash with `--before no-reset`.
- **The AP's own C6 updater** (`POST /update_c6` with `url=<release download
  base>`, e.g. `https://github.com/OpenEPaperLink/OpenEPaperLink/releases/download/2.92`)
  downloaded all three C6 files but failed with "Connection to the C6 failed"
  on the first AP. Not diagnosed yet; the C6 was flashed over USB instead.
  Watch it live on `ws://<ip>/ws` (messages are `{"console": "..."}`).
- **`apstate` is not in `/sysinfo`** on this build; `ap_version` is the useful
  field. The S3's own TFT appears in `get_db` as a tag with `hwType 0xE0`.

## Welcome cards

`tools/welcome_cards.py watch` polls an AP and pushes a one-off character card
to each new tag, sized from `resources/tagtypes/<hwType>.json` in the OEPL
checkout (red used when the panel has it). `OEPL_AP` selects the AP. The nine
tags on the first spaghetti AP got theirs on 2026-09-29.

## Boards flashed so far

| Board              | MAC ends   | State                                   |
|--------------------|------------|-----------------------------------------|
| S3 #1 + C6         | 6C:C8      | the second AP, nine tags                |
| S3 #2              | 04:00      | Yellow AP flashed, spare                |
| S3 #3              | 76:10      | Yellow AP flashed, spare                |
| C6 #2              | 73:B4:04   | radio flashed, spare                    |
| C6 #3              | 73:B3:1C   | radio flashed, spare                    |
| S3 #4, #5, #6      | 6E:64, 74:74, 7F:6C | Yellow AP flashed 2026-10-04, for gift APs |
| C6 #4, #5, #6      | 73:B2:CC, 73:BE:C8, 73:AF:54 | radio flashed 2026-10-04, for gift APs |
| S3 #7-#12          | CB:C0, 32:30, 3A:80, 7D:A8, 7A:F0, 48:F0 | Yellow AP flashed 2026-10-28 |
