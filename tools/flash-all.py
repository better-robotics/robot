#!/usr/bin/env python3
"""Flash every connected ESP32 board over USB, skipping ports that aren't one.

Loops `pio run -e <env> -t upload --upload-port <port>` over every serial
port whose USB vendor ID matches a known ESP32 bridge chip, and maps that
VID straight to the matching platformio.ini env. Same VID allowlist as
better-robotics/workbench's docs/recovery/boards.js:ESP_USB_VIDS (FTDI,
CP210x, CH340, Espressif native USB-CDC) — a hub's USB-gadget console or
any other unrelated serial device on the bus reports a different vendor ID
and gets skipped instead of getting an esptool sync thrown at it.

VID→env is a clean 1:1 here (unlike workbench's board picker, which has to
disambiguate CP210x between two boards): this fleet's esp32dev is FTDI and
esp32cam is CP210x, so vendor ID alone picks the right env. CH340 has no env
in this repo's platformio.ini (no CH340 board in the fleet) — matched but
unmapped ports are reported, not guessed at.

Usage:
    tools/flash-all.py              # flash every ESP32 found
    tools/flash-all.py --dry-run    # show the port -> env plan, flash nothing
"""
import argparse
import subprocess
import sys

try:
    import serial.tools.list_ports as list_ports
except ImportError:
    sys.exit("pyserial missing — `pip install pyserial` (or source your PlatformIO venv)")

VID_TO_ENV = {
    0x0403: "esp32dev",           # FT232R — the devkit's own FTDI
    0x10c4: "esp32cam",           # CP2102 — the CAM's USB↔UART adapter
    0x303a: "esp32c3-supermini",  # Espressif native USB-CDC/JTAG
}
VID_NAME = {0x0403: "FT232R", 0x10c4: "CP2102", 0x1a86: "CH340", 0x303a: "USB-CDC/JTAG"}
ESP_USB_VIDS = set(VID_NAME)  # includes 0x1a86 (CH340) even with no env mapped


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true", help="print the plan, flash nothing")
    args = ap.parse_args()

    # Only USB devices report a vid at all — Bluetooth-Incoming-Port, the Pi's
    # own usbmodem console, etc. come back None or with an unlisted vid.
    ports = [p for p in list_ports.comports() if p.vid is not None]
    matched = [p for p in ports if p.vid in ESP_USB_VIDS]
    skipped = [p for p in ports if p.vid not in ESP_USB_VIDS]

    for p in skipped:
        print(f"skip {p.device} (vid=0x{p.vid:04x}, {p.manufacturer or 'unknown vendor'} — not an ESP32 bridge)", flush=True)

    unmapped = [p for p in matched if p.vid not in VID_TO_ENV]
    for p in unmapped:
        print(f"skip {p.device} (vid=0x{p.vid:04x} {VID_NAME[p.vid]} — no platformio.ini env for this bridge)", flush=True)
    matched = [p for p in matched if p.vid in VID_TO_ENV]

    if not matched:
        sys.exit("no flashable ESP32 boards found on USB")

    plan = [(p, VID_TO_ENV[p.vid]) for p in matched]
    print(f"found {len(plan)} board(s):", flush=True)
    for p, env in plan:
        print(f"  {p.device} (vid=0x{p.vid:04x} {VID_NAME[p.vid]}) -> {env}", flush=True)

    if args.dry_run:
        return

    failed = []
    for p, env in plan:
        print(f"\n==== flashing {env} on {p.device} ====", flush=True)
        r = subprocess.run(["pio", "run", "-e", env, "-t", "upload", "--upload-port", p.device])
        if r.returncode != 0:
            failed.append(p.device)

    ok = len(plan) - len(failed)
    print(f"\n{ok}/{len(plan)} flashed" + (f" — failed: {', '.join(failed)}" if failed else ""), flush=True)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
