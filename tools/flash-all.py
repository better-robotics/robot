#!/usr/bin/env python3
"""Flash every connected ESP32 board over USB, skipping ports that aren't one.

Loops `pio run -e <env> -t upload --upload-port <port>` over every serial
port whose USB vendor ID matches a known ESP32 bridge chip, and maps that
VID straight to the matching platformio.ini env. Same VID allowlist as
better-robotics/workbench's docs/recovery/boards.js:ESP_USB_VIDS (FTDI,
CP210x, CH340, Espressif native USB-CDC) — a hub's USB-gadget console or
any other unrelated serial device on the bus reports a different vendor ID
and gets skipped instead of getting an esptool sync thrown at it.

VID→env is a *best-effort guess*, not a guarantee: it's 1:1 only while each VID
maps to one board. Bridge chips are commodity parts in front of arbitrary boards
— two boards can share one, and the map itself can rot when bench wiring changes
(it was inverted for weeks and misflashed both boards; re-verified 2026-07-12).
HARDWARE_SPECIFIC envs (esp32cam, whose -DHAS_CAMERA=1 mutes a non-camera board)
refuse to auto-flash on a VID collision and are reported for explicit by-port
flashing, rather than guessed at. CH340 has no env in this repo's platformio.ini
(no CH340 board in the fleet) — matched but unmapped ports are likewise reported,
not guessed at.

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

# Re-verified on the wire 2026-07-12 after boards got misflashed BOTH ways under
# the previous (inverted) map: the devkit's onboard bridge is the CP2102, and the
# FT232R is the CAM's external adapter — not the other way around.
VID_TO_ENV = {
    0x0403: "esp32cam",           # FT232R — the CAM's plug-in USB↔UART adapter
    0x10c4: "esp32dev",           # CP2102 — the devkit's onboard bridge
    0x303a: "esp32c3-supermini",  # Espressif native USB-CDC/JTAG
}
VID_NAME = {0x0403: "FT232R", 0x10c4: "CP2102", 0x1a86: "CH340", 0x303a: "USB-CDC/JTAG"}
ESP_USB_VIDS = set(VID_NAME)  # includes 0x1a86 (CH340) even with no env mapped

# Envs whose build hard-codes a specific board's hardware, so flashing one onto
# the WRONG board breaks it rather than merely losing a feature: esp32cam ships
# -DHAS_CAMERA=1, which disables motors + the button (camera.c owns those pins).
# The VID→env map is a *guess* — safe only when the VID uniquely identifies the
# board. FT232R and CP2102 adapters are commodity parts that can sit in front of
# ANY board (2026-07-12: the map above was inverted for weeks and misflashed both
# bench boards). So we refuse to auto-guess these envs on a VID collision and make
# the operator flash them by port, instead of silently muting a motor robot.
HARDWARE_SPECIFIC = {"esp32cam"}


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

    # A hardware-specific env with >1 candidate port is an unresolved guess —
    # drop those ports from the auto plan and tell the operator to flash by port.
    from collections import Counter
    env_counts = Counter(env for _, env in plan)
    ambiguous = {env for env in HARDWARE_SPECIFIC if env_counts.get(env, 0) > 1}
    if ambiguous:
        for env in sorted(ambiguous):
            devs = [p.device for p, e in plan if e == env]
            print(f"AMBIGUOUS: {len(devs)} ports map to {env} ({', '.join(devs)}) — "
                  f"can't tell which is the real board. Flash it explicitly:\n"
                  f"    pio run -e {env} -t upload --upload-port <the-right-port>\n"
                  f"    pio run -e <other-env> -t upload --upload-port <the-other-port>",
                  flush=True)
        plan = [(p, env) for p, env in plan if env not in ambiguous]

    if not plan:
        sys.exit("nothing left to auto-flash (all candidates were ambiguous)")

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
