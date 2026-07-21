# Web flasher

Browser firmware flashing over USB (Web Serial + [esptool-js](https://github.com/espressif/esptool-js),
vendored in `vendor/` for the `script-src 'self'` CSP). Desktop Chrome/Edge only.

**esptool-js is pinned to 0.5.7** (`vendor/VERSION`): 0.6.0's compressed-write path
fails on images past ~1.1 MB (the unified robot image is ~1.5 MB) — 0.5.x flashes
it fine, matching the `workbench` recovery flasher. Rebuild the self-contained
bundle with esbuild:

```
echo 'export { ESPLoader, Transport } from "esptool-js";' > entry.js
npm install esptool-js@0.5 && npx esbuild entry.js --bundle --format=esm --minify --outfile=vendor/esptool-bundle.js
```

`flash.js` is a module — `mountFlasher(el)` — mounted by the site's single landing
page (`../index.html`); it is not its own page. Two-step: Connect identifies the
chip, a second click writes its image. A classic ESP32 adds a board picker in
between: the devkit and the AI-Thinker ESP32-CAM are the same die, so esptool's
`CHIP_NAME` can't tell them apart — and their images swap pin ownership (the CAM
build's camera owns pins the devkit build gives to motors/button), so the user
must say which board it is; Flash stays disabled until they do. A completed flash
rolls straight into the serial monitor — the board's output at 115200 (raw Web
Serial — esptool only writes; the two share the port, so they interlock), shown
collapsed under the next steps. There is no standalone log entry point: nobody
arrives here to read logs, and after a flash they are already streaming. The
secondary button only exits a held state (Disconnect / Stop logs) and is absent
otherwise. Asset paths resolve against the module (`import.meta.url`), so it
works wherever it's mounted.

A Raspberry Pi hub is **not** flashed here — it's a full SD-card image (built
from the hub repo), not an ESP chip. The landing page notes that path separately.

## This site lives beside the firmware it flashes

`web/site/` is the whole GitHub Pages site this repo serves at
**https://better-robotics.github.io/robot/** — the flasher, one page, no build
step. It sits in `robot` on purpose: the flasher exists only to write `robot`'s
firmware, so its board table, its offsets, and the images all originate here, and
one commit moves the flasher and the firmware together. The org apex
(`better-robotics.github.io`) is a thin landing page that links here; it carries
no flasher and no board names.

## Images (`bin/<target>/`)

**Not in git.** The `pages` job in `.github/workflows/firmware.yml` lays them out
under `bin/<target>/` from the *same CI run* that built them, so the page serves
them same-origin without the repo ever carrying a binary — and without fetching
across a repo boundary. The bytes go straight from the `build` job's artifacts to
the deployed site.

They are built from a clean clone, which is the point rather than a convenience:
a working tree carries gitignored build inputs (`src/wifi_creds.h`) that a clean
clone does not, so an image built locally is a function of the machine, and only
a CI image is a function of the commit its stamp names. The `pages` job refuses
to deploy an image that does not contain `wifi_creds.example.h`'s placeholder
SSID — a positive control at the serve boundary, since proving a specific value
*absent* would mean naming it in a public repo. (The `publish` job runs the same
control over the release it cuts; the two guard different artifacts from one
source.)

One **unified image** per chip: it boots as a robot and carries the on-chip hub
role in the same binary. The separate `hub-esp32` image was retired 2026-07-09
when the ESP32 hub folded into the robot firmware.

| target | chip | source build |
|--------|------|--------------|
| `robot-esp32dev` | ESP32 (picker: DevKit) | `pio run -e esp32dev` → `.pio/build/esp32dev/{bootloader,partitions,firmware}.bin` |
| `robot-esp32cam` | ESP32 (picker: ESP32-CAM) | `pio run -e esp32cam` → same layout |
| `robot-c3` | ESP32-C3 | `pio run -e esp32c3-supermini` → same layout |

## Flash offsets (in `flash.js` `IMAGES`)

Chip-specific — the ESP32 second-stage bootloader is at `0x1000`, but the RISC-V
parts (C3/S3/C6…) put it at `0x0`. Partition table and app match across both.
The ESP32 column covers both `robot-esp32dev` and `robot-esp32cam` (same chip,
same `partitions.csv`).

| part | ESP32 | ESP32-C3 |
|------|-------|----------|
| bootloader | `0x1000` | `0x0` |
| partition table | `0x8000` | `0x8000` |
| otadata (0x2000 of `0xff`) | `0xd000` | `0xd000` |
| app (`ota_0`) | `0x10000` | `0x10000` |

**A/B, no factory** — `partitions.csv` is `ota_0`/`ota_1` (since 2026-07-16), so
the bootloader picks its slot from `otadata`. This page writes the app to `ota_0`,
and therefore must also reset `otadata`: a board that has taken an OTA push has
`otadata` → `ota_1`, and flashing `ota_0` under it would boot the OLD image while
the page reported success. `0xff` is the erased state, which the bootloader reads
as invalid and falls back to `ota_0`.

`nvs` (`0x9000`) is not in the write list, so a board already on this table keeps
its stored name and Wi-Fi across a reflash. A board still on the old factory
table does not: its `nvs` ran to `0xe000`, so `0xd000` was its last NVS page and
this write erases it. Re-provisioning on the first flash after the repartition is
expected, not a failure — `src/main.c` erases and retries when NVS won't init, so
the board comes up clean rather than stuck.

Offsets are still hardcoded in `flash.js` rather than carried in `manifest.json`
— so this table and `partitions.csv` are two copies of one fact, and nothing
checks them against each other. Change the table there and this page flashes to
the old offsets until someone edits it by hand.

## Updating an image

Nothing to do — land the change on `main`. `firmware.yml` rebuilds the images and
its `pages` job redeploys this site in the same run, so the page follows `main` on
its own, with no rolling tag to poll and no cross-repo trigger to keep alive.
`workflow_dispatch` on `firmware` forces a rebuild-and-redeploy.

Offsets live in `flash.js`. Adding a target means a new env in the build matrix
and the `pages` job's layout step, plus an `IMAGES` entry here (a variant, if the
chip is ambiguous).
