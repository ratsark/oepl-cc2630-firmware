#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Nathan Bigelow
"""
Flash the TG-GR6000N (CC2630) through an OpenEPaperLink Tag-Flasher
(Wemos S2 mini running the OEPL ESP32_Flasher firmware, v50 or later).

The S2 Tag-Flasher has no native CC2630 mode. What it does have is a
serial-passthrough mode (CMD_PASS_THROUGH) that turns it into a plain
115200-baud USB-UART bridge on its TXD/RXD wires, plus control of the tag's
power rail (VCC wires) and of the TEST wire. That is everything the CC2630 ROM
bootloader needs:

    S2 wire   ->  tag pad
    VCC       ->  BAT      (batteries OUT of the tag; the S2 powers it)
    GND       ->  GND
    TXD       ->  RXD      (crossover)
    RXD       ->  TXD      (crossover)
    TEST      ->  TI_DN    (D/L, DIO11: LOW at power-on enters the bootloader)
    RESET     ->  (leave unconnected)

If your flasher has no TEST wire, jumper the tag's TI_DN pad to GND by hand for
the whole run and pass --manual-dl.

Why not just run cc2538-bsl against the S2's port?  The S2 firmware leaves
passthrough mode on any CDC line-coding change or DTR drop, and cc2538-bsl does
both when it opens the port and toggles DTR/RTS.  So this script opens the port
once, drives the S2 into passthrough, then hands the *same open port* to
cc2538-bsl's CommandInterface and never touches DTR or the baud rate until the
job is done.

Sequence:
  1. tag power OFF, TEST (D/L) LOW
  2. tag power ON   -> ROM sees DIO11 low, stays in the bootloader
  3. passthrough    -> sync, identify, erase, write, CRC32 verify, readback
  4. leave passthrough (DTR drop), tag power OFF

Then pull the tag off the jig and put the batteries in.  DIO11 is no longer
driven, so the ROM boots the application and you get the splash screen.

Usage:
  tools/flash_s2.py                                # flash binaries/Tag_FW_CC2630_TG-GR6000N.bin
  tools/flash_s2.py -p /dev/cu.usbmodem01 FILE.bin
  tools/flash_s2.py --probe                        # bootloader handshake + chip/IEEE address, no write
  tools/flash_s2.py --read out.bin                 # dump the whole 128 KB flash (slow, ~1 min)

Needs pyserial (and cc2538-bsl checked out at ~/Code/cc2538-bsl, or set
CC2538_BSL to its cc2538_bsl.py).  The cc2538-bsl venv has both:
  source ~/Code/cc2538-bsl/venv/bin/activate
"""

import argparse
import glob
import os
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial not installed. Try: source ~/Code/cc2538-bsl/venv/bin/activate")

# ---- locate cc2538-bsl and import it as a module -----------------------------
_BSL_CANDIDATES = [
    os.environ.get("CC2538_BSL", ""),
    os.path.expanduser("~/Code/cc2538-bsl/cc2538_bsl/cc2538_bsl.py"),
    os.path.expanduser("~/Code/cc2538-bsl/cc2538-bsl.py"),
]
_bsl_path = next((p for p in _BSL_CANDIDATES if p and os.path.isfile(p)), None)
if not _bsl_path:
    sys.exit("cc2538-bsl not found; clone https://github.com/JelmerT/cc2538-bsl to ~/Code "
             "or set CC2538_BSL=/path/to/cc2538_bsl.py")
sys.path.insert(0, os.path.dirname(_bsl_path))
import importlib.util
_spec = importlib.util.spec_from_file_location("cc2538_bsl", _bsl_path)
bsl = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(bsl)

# ---- OEPL Tag-Flasher (S2) command protocol ----------------------------------
CMD_GET_VERSION = 1
CMD_SET_POWER = 13
CMD_SET_TESTP = 14
CMD_PASS_THROUGH = 50
CMD_SELECT_PORT = 70
PORT_EXTERNAL = 1

FLASH_SIZE = 128 * 1024
CCFG_OFFSET = FLASH_SIZE - 88


def s2_send(ser, cmd, data=b""):
    pkt = bytes([cmd]) + len(data).to_bytes(4, "big") + data
    crc = (0xAB34 + sum(pkt)) & 0xFFFF
    ser.write(b"AT" + pkt + crc.to_bytes(2, "big"))
    ser.flush()


def s2_wait(ser, expect, timeout=3.0):
    """Read one 'AT' reply frame from the flasher; return its payload."""
    ser.timeout = timeout
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        b = ser.read(1)
        if not b:
            continue
        buf += b
        i = buf.find(b"AT")
        if i < 0:
            buf = buf[-1:]
            continue
        if len(buf) < i + 7:
            continue
        cmd = buf[i + 2]
        length = int.from_bytes(buf[i + 3:i + 7], "big")
        need = i + 7 + length + 2
        if len(buf) < need:
            buf += ser.read(need - len(buf))
            if len(buf) < need:
                break
        payload = buf[i + 7:i + 7 + length]
        if cmd == expect:
            return payload
        buf = buf[need:]
    raise RuntimeError("flasher did not answer command %d" % expect)


def s2_cmd(ser, cmd, data=b"", reply=True):
    s2_send(ser, cmd, data)
    return s2_wait(ser, cmd) if reply else None


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))
    if not ports:
        sys.exit("no S2 mini found (/dev/cu.usbmodem* or /dev/ttyACM*); pass -p")
    return ports[0]


def leave_passthrough(ser):
    # The S2 firmware calls resetFlasherState() when DTR goes low.
    ser.dtr = False
    time.sleep(0.2)
    ser.dtr = True
    time.sleep(0.2)
    ser.reset_input_buffer()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("firmware", nargs="?",
                    default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                         "binaries", "Tag_FW_CC2630_TG-GR6000N.bin"))
    ap.add_argument("-p", "--port", help="S2 mini serial port (default: first /dev/cu.usbmodem*)")
    ap.add_argument("--probe", action="store_true",
                    help="only enter the bootloader and identify the chip; write nothing")
    ap.add_argument("--read", metavar="OUT.bin", help="dump the whole flash to OUT.bin instead of writing")
    ap.add_argument("--manual-dl", action="store_true",
                    help="don't drive the TEST wire; you are holding TI_DN to GND yourself")
    ap.add_argument("--keep-power", action="store_true",
                    help="leave the tag powered by the S2 when done (default: power off)")
    args = ap.parse_args()

    port = args.port or find_port()
    do_write = not (args.probe or args.read)

    if do_write:
        if not os.path.isfile(args.firmware):
            sys.exit("firmware not found: %s" % args.firmware)
        fw = bsl.FirmwareFile(args.firmware)
        if len(fw.bytes) != FLASH_SIZE:
            sys.exit("expected a full 128 KB image with CCFG (got %d bytes); "
                     "this is not the _ota.bin" % len(fw.bytes))
        bl_config = fw.bytes[CCFG_OFFSET + 48:CCFG_OFFSET + 52]
        image_valid = fw.bytes[CCFG_OFFSET + 68:CCFG_OFFSET + 72]
        # Refuse to brick: byte 51 (BOOTLOADER_ENABLE) and byte 48 (BL_ENABLE)
        # must both be 0xC5 or the UART route is gone forever (LESSONS_LEARNED.md).
        if bl_config[3] != 0xC5 or bl_config[0] != 0xC5:
            sys.exit("REFUSING: CCFG BL_CONFIG is %s, bootloader/backdoor would be disabled"
                     % bl_config.hex())
        if image_valid != b"\x00\x00\x00\x00":
            sys.exit("REFUSING: CCFG IMAGE_VALID is %s, app would never boot" % image_valid.hex())
        print("Firmware: %s (%d bytes, CCFG ok, BL_PIN=DIO%d)" % (args.firmware, len(fw.bytes), bl_config[1]))

    print("S2 port:  %s" % port)
    ser = serial.Serial(port, 115200, timeout=3)   # DTR/RTS come up asserted; leave them alone
    time.sleep(0.3)
    ser.reset_input_buffer()

    ver = int.from_bytes(s2_cmd(ser, CMD_GET_VERSION), "big")
    print("Tag-Flasher firmware version %d" % ver)
    if ver < 50:
        sys.exit("Tag-Flasher v50 or later needed for passthrough")

    # 1. everything off, D/L low
    s2_send(ser, CMD_SELECT_PORT, bytes([PORT_EXTERNAL]))   # no reply frame for this one
    s2_cmd(ser, CMD_SET_POWER, bytes([0]))
    if not args.manual_dl:
        s2_cmd(ser, CMD_SET_TESTP, bytes([0]))
        print("TEST wire LOW (D/L held low)")
    else:
        print("Assuming TI_DN is jumpered to GND by hand")
    time.sleep(0.5)

    # 2. power on -> ROM bootloader
    s2_cmd(ser, CMD_SET_POWER, bytes([1]))
    print("Tag power ON via S2 VCC wires")
    time.sleep(0.3)

    # 3. passthrough
    s2_send(ser, CMD_PASS_THROUGH)
    time.sleep(0.3)
    ser.reset_input_buffer()                      # eats the ">>>" banner

    ok = False
    try:
        cmd = bsl.CommandInterface()
        cmd.sp = ser
        ser.timeout = 1.0
        ser.write_timeout = 5.0

        print("Syncing with ROM bootloader...")
        try:
            synced = cmd.sendSynch()
        except bsl.CmdException:
            synced = False
        if not synced:
            raise RuntimeError("no answer on synch. Check: batteries out, TI_DN low at power-on, "
                               "TXD/RXD crossed (try swapping), GND common.")
        chip_id = cmd.cmdGetChipId()
        device = bsl.CC26xx(cmd)     # prints chip, flash/SRAM, IEEE address
        if device.size != FLASH_SIZE:
            raise RuntimeError("unexpected flash size %d" % device.size)

        if args.probe:
            vt = bytearray()
            for a in range(0, 16, 4):
                vt += cmd.cmdMemReadCC26xx(a)
            print("Vector table: SP=0x%08X Reset=0x%08X" %
                  (int.from_bytes(vt[0:4], "little"), int.from_bytes(vt[4:8], "little")))
            ccfg = bytearray()
            for a in range(CCFG_OFFSET + 48, CCFG_OFFSET + 52, 4):
                ccfg += cmd.cmdMemReadCC26xx(a)
            print("CCFG BL_CONFIG on tag: %s" % ccfg.hex())
            ok = True
        elif args.read:
            print("Reading %d bytes..." % FLASH_SIZE)
            data = bytearray()
            for a in range(0, FLASH_SIZE, 4):
                data += cmd.cmdMemReadCC26xx(a)
                if a % 8192 == 0:
                    print("  %3d%%" % (100 * a // FLASH_SIZE), end="\r", flush=True)
            with open(args.read, "wb") as f:
                f.write(data)
            print("Wrote %s (%d bytes)" % (args.read, len(data)))
            ok = True
        else:
            print("Mass erase...")
            if not device.erase():
                raise RuntimeError("erase failed")
            print("Writing %d bytes..." % len(fw.bytes))
            if not cmd.writeMemory(0, fw.bytes):
                raise RuntimeError("write failed")
            print("Verifying CRC32...")
            crc_local = fw.crc32()
            crc_target = device.crc(0, len(fw.bytes))
            if crc_local != crc_target:
                raise RuntimeError("CRC mismatch: local 0x%08x, tag 0x%08x" % (crc_local, crc_target))
            print("  CRC32 match: 0x%08x" % crc_local)
            print("Readback spot-check (first 256 bytes)...")
            rb = bytearray()
            for a in range(0, 256, 4):
                rb += cmd.cmdMemReadCC26xx(a)
            if bytes(rb) != bytes(fw.bytes[:256]):
                raise RuntimeError("vector table readback mismatch")
            print("  OK")
            ok = True
    except (RuntimeError, bsl.CmdException) as e:
        print("ERROR: %s" % e)
    finally:
        # 4. leave passthrough and shut the tag down
        try:
            leave_passthrough(ser)
            if not args.keep_power:
                s2_cmd(ser, CMD_SET_POWER, bytes([0]))
                print("Tag power OFF")
        except Exception as e:  # don't mask the real error
            print("(cleanup: %s)" % e)
        ser.close()

    if ok:
        print("\n=== PASS ===")
        if do_write:
            print("Take the tag off the jig, put the batteries in: it should show the FW splash.")
    else:
        print("\n=== FAIL ===")
        sys.exit(1)


if __name__ == "__main__":
    main()
