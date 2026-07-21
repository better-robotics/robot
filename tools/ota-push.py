#!/usr/bin/env python3
"""Push a firmware image to boards over the classroom Wi-Fi (POST /ota).

The counterpart to flash-all.py: that one walks USB ports, this one walks
hostnames. A board only needs USB for its FIRST flash (and for the one
repartition onto the A/B table); after that it is reachable at
robot-<id>.local, or hub.local for a dedicated hub.

Auth is the instructor password — the same secret the broker's session auth
gates on, sent over HTTP Basic. Set INSTRUCTOR_PASS in the environment; the
firmware's built-in default is "change-me" on a board that never had one set.
It is passed as an env var and never as an argument, so it stays out of `ps`
and shell history.

Safety is the board's, not this script's: partitions.csv is ota_0 + ota_1 and
the bootloader boots a pushed image on trial. An image that does not survive
its self-test is reverted automatically, with no USB trip. This script reports
what the board said; it cannot itself confirm the new image is good.

Usage:
    INSTRUCTOR_PASS=... tools/ota-push.py --host robot-a044.local .pio/build/esp32dev/firmware.bin
    INSTRUCTOR_PASS=... tools/ota-push.py --host a.local --host b.local <bin>
    tools/ota-push.py --host robot.local <bin> --dry-run
"""
import argparse
import base64
import json
import os
import sys
import urllib.error
import urllib.request

TIMEOUT_S = 180   # a 1.3 MB image over one shared AP radio is not a fast transfer


def _post(host, blob, password, timeout):
    url = "http://%s/ota" % host
    auth = base64.b64encode(("instructor:%s" % password).encode()).decode()
    return urllib.request.Request(
        url, data=blob, method="POST",
        headers={
            "Authorization": "Basic " + auth,
            "Content-Type": "application/octet-stream",
            "Content-Length": str(len(blob)),
        },
    ), timeout


def preflight(host, password, timeout=30):
    # 30, not 10: a healthy esp32cam answered its first cold request in 5.7 s on
    # the bench (2026-07-16) and its next two in 0.2 s and 0.06 s — that board
    # runs a second httpd for the camera and is slowest exactly when nothing is
    # wrong with it. A 10 s budget left under 2x margin on the slowest board
    # observed, which turns radio contention in a full classroom into "timed
    # out" against a board that is fine. This costs nothing when healthy.
    """Check auth with an empty body before sending megabytes.

    The board checks auth BEFORE it looks at the image, so a zero-byte push
    answers 401 for a bad password and 400 ("image missing") for a good one —
    which makes it a free auth probe.

    Without this, a wrong password is a bad experience and a misleading one: the
    board rejects at the first byte while the client is still streaming 1.3 MB,
    closes the socket, and the client surfaces a broken pipe instead of the
    board's actual reason. Reachability gets tested here too, so an offline
    board fails in a second rather than after a long upload.

    Returns (ok, message); ok=True means the credentials are good.
    """
    req, t = _post(host, b"", password, timeout)
    try:
        urllib.request.urlopen(req, timeout=t)
        return True, ""          # unexpected 2xx, but auth clearly passed
    except urllib.error.HTTPError as e:
        if e.code == 401:
            try:
                return False, json.loads(e.read().decode()).get("error", "auth rejected")
            except Exception:
                return False, "auth rejected"
        return True, ""          # 400 = auth passed, image absent. Expected.
    except urllib.error.URLError as e:
        return False, "unreachable (%s)" % e.reason
    except TimeoutError:
        return False, "timed out"


def push(host, blob, password, timeout=TIMEOUT_S):
    """POST the image. Returns (ok: bool, message: str)."""
    req, timeout = _post(host, blob, password, timeout)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            body = json.loads(r.read().decode() or "{}")
        if body.get("ok"):
            return True, "rebooting into %s" % body.get("slot", "the new slot")
        return False, body.get("error", "refused, no reason given")
    except urllib.error.HTTPError as e:
        # The board's own JSON reason is far better than the status line —
        # "instructor auth required" beats "HTTP Error 401".
        try:
            return False, json.loads(e.read().decode()).get("error", str(e))
        except Exception:
            return False, "HTTP %s" % e.code
    except urllib.error.URLError as e:
        return False, "unreachable (%s)" % e.reason
    except TimeoutError:
        return False, "timed out after %ds" % timeout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image", help="path to firmware.bin")
    ap.add_argument("--host", action="append", required=True, metavar="HOST",
                    help="board hostname or IP; repeat for several")
    ap.add_argument("--dry-run", action="store_true", help="print the plan, push nothing")
    args = ap.parse_args()

    try:
        blob = open(args.image, "rb").read()
    except OSError as e:
        sys.exit("cannot read %s: %s" % (args.image, e))

    # The ESP32 image magic. A firmware.elf or a .bin from another project
    # would be rejected by esp_ota_end anyway, but only after pushing 1.3 MB
    # over the air first — checking one byte here is free.
    if not blob or blob[0] != 0xE9:
        sys.exit("%s is not an ESP32 firmware image (no 0xE9 magic)" % args.image)

    print("%s: %d bytes" % (args.image, len(blob)))
    if args.dry_run:
        for h in args.host:
            print("  would push -> %s" % h)
        return 0

    password = os.environ.get("INSTRUCTOR_PASS")
    if not password:
        sys.exit("INSTRUCTOR_PASS not set — export it rather than passing it as an argument")

    failures = 0
    for h in args.host:
        print("  %-24s checking..." % h, end="", flush=True)
        ok, msg = preflight(h, password)
        if not ok:
            print("\r  %-24s FAILED — %s" % (h, msg))
            failures += 1
            continue
        print("\r  %-24s pushing... " % h, end="", flush=True)
        ok, msg = push(h, blob, password)
        print("\r  %-24s %s" % (h, ("OK — " + msg) if ok else ("FAILED — " + msg)))
        failures += not ok

    if failures:
        print("\n%d of %d failed. A board that refused the push is still running its"
              "\nold image — a failed push is not a bricked board." % (failures, len(args.host)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
