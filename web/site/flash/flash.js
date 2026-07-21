// Web-Serial firmware flasher — writes a prebuilt image to an ESP32 over USB,
// no toolchain install. Uses esptool-js (vendored, self-hosted for the page's
// script-src 'self' CSP). Desktop Chrome/Edge only — Web Serial isn't in
// Firefox/Safari or any mobile browser.
//
// Two-step: the first click ("Connect to flash") opens the board and shows what
// it is — its label then becomes "Flash <chip>" and a second click writes
// the firmware, then the serial monitor auto-starts so you watch the fresh
// firmware boot. Mounted by the site's single landing page (index.html) via
// mountFlasher(); asset paths resolve relative to THIS module (import.meta.url),
// so it doesn't care which page hosts it.
//
// A Raspberry Pi hub is NOT flashed here — it's a full SD-card image, not an ESP
// chip; see the Pi note on the page (built from the hub repo).
import { ESPLoader, Transport } from "./vendor/esptool-bundle.js";

// Flash offsets are chip-specific: the original ESP32 (and S2) second-stage
// bootloader lives at 0x1000, but the newer parts (C3/S3/C6…) put it at 0x0 — the
// S3 is Xtensa like the classic chip, so this is a silicon-generation split, not a
// core one. Partition table and app are the same across both. Sourced from each
// build's flasher_args.json / PlatformIO layout — see flash/README.md.
// One unified image per chip — it boots as a robot and carries the on-chip hub
// role in the same binary. We detect the chip on connect (esptool reports
// CHIP_NAME) and key by that exact string. (The standalone hub-esp32 image was
// retired 2026-07-09 when the ESP32 hub folded into the robot firmware.)
//
// An entry with `variants` is a question, not an answer: a plain devkit and an
// AI-Thinker ESP32-CAM are the same classic-ESP32 die, so chip detection can't
// tell them apart — and the two images swap pin ownership (the CAM build's
// camera owns pins that are motor/button pins on the devkit build). Only the
// person at the desk can say which board it is, so the UI asks and the Flash
// button waits; a silent default is how a CAM board spent a day announcing
// itself as a devkit.
const IMAGES = {
  "ESP32": {
    variants: [
      { name: "DevKit", hint: "plain board, no camera",
        parts: [["bin/robot-esp32dev/bootloader.bin", 0x1000],   // xtensa: 2nd-stage bootloader at 0x1000
                ["bin/robot-esp32dev/partitions.bin", 0x8000],
                ["bin/robot-esp32dev/firmware.bin",   0x10000]] },
      { name: "ESP32-CAM", hint: "camera module on the front",
        parts: [["bin/robot-esp32cam/bootloader.bin", 0x1000],
                ["bin/robot-esp32cam/partitions.bin", 0x8000],
                ["bin/robot-esp32cam/firmware.bin",   0x10000]] },
    ],
  },
  "ESP32-C3": {
    parts: [["bin/robot-esp32c3/bootloader.bin", 0x0],             // risc-v: bootloader at 0x0
            ["bin/robot-esp32c3/partitions.bin", 0x8000],
            ["bin/robot-esp32c3/firmware.bin",   0x10000]],
  },
  // One S3 board in the fleet (the Freenove S3-CAM), so the chip name alone is
  // unambiguous — no variant question like the classic ESP32's. Bootloader at 0x0
  // (the S3, like the C3, isn't an original-ESP32 0x1000 part).
  "ESP32-S3": {
    parts: [["bin/robot-esp32s3cam/bootloader.bin", 0x0],
            ["bin/robot-esp32s3cam/partitions.bin", 0x8000],
            ["bin/robot-esp32s3cam/firmware.bin",   0x10000]],
  },
};
const SUPPORTED = Object.keys(IMAGES).join(", ");

// Reset otadata on every flash, or the board boots whatever an earlier OTA push
// chose and this page says "✓ Flashed" over the OLD firmware — the one lie a
// recovery path must never tell.
//
// robot's partitions.csv is A/B with NO factory slot (since 2026-07-16): the
// bootloader reads otadata to pick ota_0 or ota_1. This flasher writes the app
// to 0x10000 = ota_0. A board that has taken an OTA has otadata -> ota_1, so
// without this it flashes ota_0 and then boots ota_1. 0xff is the erased state;
// an invalid otadata makes the bootloader fall back to ota_0 — exactly the slot
// we just wrote. Surgical rather than eraseAll:true so a reflash doesn't wipe
// every board's stored name and Wi-Fi (nvs at 0x9000 is left alone). A board
// crossing over from the old factory table is the exception — 0xd000 was nvs's
// last page there, so its config goes; robot's src/main.c erases and retries
// when NVS won't init, so it re-provisions rather than sticking.
//
// COUPLED TO THE IMAGES: 0xd000 is otadata only under the A/B table. Under the
// older factory table it was inside nvs, and this write would clobber nvs's last
// page and phy_init. That is safe only because the page and its images deploy
// together — deploy.yml fetches firmware-latest as it assembles the site, so the
// bytes here and the table they assume always land in one step. If this file
// ever ships without a matching release, that invariant is gone.
const OTADATA_ADDR = 0xd000;
const OTADATA_SIZE = 0x2000;

// esptool-js wants each part's bytes as a *binary string* (one char per byte),
// not an ArrayBuffer. Chunk the char-code conversion so a ~1.4 MB image doesn't
// blow the argument limit of String.fromCharCode(...spread). `url` is resolved
// against import.meta.url by the caller, so the fetch works from any page.
async function fetchBinString(url) {
  // `no-cache` = always revalidate (conditional GET → 304 when unchanged), never
  // re-download. Pages serves these max-age=600, so without it a returning
  // browser within 10 min of a redeploy would flash the PREVIOUS image from its
  // HTTP cache while the page shows the new version — the deploy caught up, the
  // client didn't. Revalidation costs one round-trip and cannot go stale.
  const res = await fetch(url, { cache: "no-cache" });
  if (!res.ok) throw new Error(`couldn't load ${url} (${res.status})`);
  const bytes = new Uint8Array(await res.arrayBuffer());
  let out = "";
  for (let i = 0; i < bytes.length; i += 0x8000)
    out += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
  return out;
}

export function mountFlasher(root) {
  const supported = "serial" in navigator;
  root.innerHTML = `
    <div class="fl-actions">
      <button id="fl-go"${supported ? "" : " disabled"}>Connect to flash</button>
      <button id="fl-mon" class="fl-secondary" hidden></button>
    </div>
    <fieldset id="fl-pick" class="fl-pick" hidden></fieldset>
    <div class="fl-status-wrap">
      <p id="fl-status" class="fl-status" role="status"></p>
      <p id="fl-alert" class="fl-status fl-err" role="alert"></p>
    </div>
    <div id="fl-bar" class="fl-bar" hidden><span id="fl-fill"></span></div>
    <div id="fl-next" class="fl-next" hidden>
      <p class="fl-next-title">Next steps</p>
      <ol class="fl-next-steps">
        <li>Join the board's own Wi-Fi network — open, named <strong>robot-XXXX</strong>.</li>
        <li>Open <strong>http://robot.local/</strong> to drive it and set it up.</li>
      </ol>
      <p class="fl-next-note">In a classroom with a hub running, the board joins the hub by
        itself and shows up on the hub's dashboard instead.</p>
    </div>
    <details id="fl-details" class="fl-details" hidden>
      <summary>Board log</summary>
      <pre id="fl-log" class="fl-log"></pre>
    </details>
    ${supported ? `<p class="fl-help"><button type="button" popovertarget="fl-driver-help">Board not showing up?</button></p>
    <div id="fl-driver-help" class="help-popover" popover>
      <strong>Nothing listed in the port picker</strong>
      Boards with a CP2102 USB chip (most classic ESP32 devkits, the ESP32-CAM's
      CAM-MB adapter) need the one-time
      <a href="https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers" target="_blank" rel="noopener">Silicon Labs driver</a>.
      The ESP32-C3 has native USB — no driver. The ESP32-S3 CAM uses a CH343 (WCH)
      chip, built into current macOS and Windows; only an older OS needs the
      <a href="https://www.wch-ic.com/downloads/CH343SER_ZIP.html" target="_blank" rel="noopener">WCH driver</a>.
      <strong>Connecting stalls</strong>
      Hold the board's BOOT button while clicking, release once it connects.
    </div>` : `<p class="fl-unsupported">This browser can't flash or read
      logs over USB — Web Serial needs desktop <strong>Chrome</strong> or
      <strong>Edge</strong>. Open this page there, or flash from the command line.</p>`}`;

  const BOOT_BAUD = 115200;   // esptool link speed — the ROM bootloader's own default
  const MON_BAUD = 115200;    // the firmware's monitor baud — same number, unrelated reason

  const $ = (id) => root.querySelector("#" + id);
  const go = $("fl-go"), mon = $("fl-mon");
  const status = $("fl-status"), alertEl = $("fl-alert");
  const bar = $("fl-bar"), fill = $("fl-fill"), logEl = $("fl-log");
  const details = $("fl-details"), next = $("fl-next"), pick = $("fl-pick");

  // Provenance stamp — shows only what bin/manifest.json verifies (written by
  // robot's firmware.yml when it publishes the images this page deploys). No
  // manifest, no claim. Fills a page-provided #fl-version slot (metadata, so the
  // host page decides where it sits — the landing page's footer); no slot, no
  // stamp.
  //
  // The sha is a link because a stamp naming bytes the reader cannot inspect is
  // a claim, and an unverifiable claim is how a stale image sat here unnoticed.
  //
  // It resolves to the release, because this page flashes an artifact: prebuilt
  // .bin files, written at fixed offsets, never compiled here. The release is
  // where those exact files can be downloaded and compared against a board dump,
  // which is what verifying the stamp means. /commit/ renders a diff of whatever
  // last landed — a CI-only commit still rebuilds and republishes, so it shows a
  // file nobody flashed. /tree/ at least names the right snapshot, but answers
  // with source for a page that never ships source.
  //
  // The tag rolls, so a page that failed to redeploy could point at newer bytes
  // than it flashed. That stays visible rather than silent: the release notes
  // name their commit, so it disagrees with the sha printed here. The trade buys
  // the only link that hands over the artifact itself.
  //
  // The sha is not decoration: the build bakes it into the ESP-IDF app
  // descriptor, so the board self-reports this exact string. Two images built
  // from the same source at different commits differ by precisely those bytes.
  fetch(new URL("bin/manifest.json", import.meta.url), { cache: "no-cache" })
    .then((r) => (r.ok ? r.json() : null))
    .then((m) => {
      if (!m?.commit) return;
      const el = document.getElementById("fl-version");
      if (!el) return;
      const repo = (m.source || "").split("/").pop() || "robot";
      // Shape-check both before they reach an href: the manifest is ours, but a
      // URL assembled from fetched data is not the place to find that out.
      const sha = /^[0-9a-f]{7,40}$/.test(m.commit) ? m.commit : null;
      const src = /^[\w.-]+\/[\w.-]+$/.test(m.source || "") ? m.source : null;
      const rel = /^[\w.-]+$/.test(m.release || "") ? m.release : null;
      el.replaceChildren("Firmware: ");
      // No release named, no artifact to point at — fall back to the source
      // snapshot rather than printing a link that guesses at a tag.
      const href = src && (rel ? `https://github.com/${src}/releases/tag/${rel}`
                              : sha ? `https://github.com/${src}/tree/${sha}` : null);
      if (href && sha) {
        const a = document.createElement("a");
        a.href = href;
        a.textContent = `${repo}@${sha}`;
        a.rel = "noopener";
        el.append(a);
      } else {
        el.append(`${repo}@${m.commit}`);
      }
      if (m.refreshed) el.append(` · ${m.refreshed}`);
      el.hidden = false;
      // No staleness check: this site is built and deployed in the SAME CI run
      // as the images it serves (firmware.yml's `pages` job), so the page and
      // its bytes are always one commit. The apex used to poll robot's rolling
      // release on a schedule and could lag it — that gap, and the api.github.com
      // freshness probe that made it visible, are gone with the repo boundary
      // that created them.
    })
    .catch(() => {});

  // Auto-scroll the log ONLY when the reader is already pinned to the bottom —
  // otherwise a chatty serial stream yanks them back down while they scroll up
  // to read earlier output. Measure before appending; re-pin after.
  const atBottom = () => logEl.scrollHeight - logEl.scrollTop - logEl.clientHeight < 4;
  // The disclosure reveals itself (collapsed) on first content and pops open on
  // an error — diagnostics stay one keypress away without dominating the card.
  const log = (line) => {
    const stick = atBottom();
    details.hidden = false;
    logEl.textContent += line + "\n";
    if (stick) logEl.scrollTop = logEl.scrollHeight;
    if (line.startsWith("ERROR:")) details.open = true;
  };
  // Failures announce assertively so a screen-reader user hears them mid-task
  // without waiting out the current polite message; everything else stays polite.
  // Two regions rather than one whose politeness flips: assistive tech registers
  // a live region's politeness when the node enters the accessibility tree, and
  // reads an attribute change batched with a text change in the same task
  // inconsistently. Both sit primed and empty in static markup, neither ever
  // changes politeness, and a message writes one while clearing the other — so
  // only the current message is ever live. The error path is the one that has to
  // land: a board that won't connect is exactly who needs CABLE_HELP.
  const say = (msg, kind = "") => {
    if (kind === "err") { status.textContent = ""; alertEl.textContent = msg; return; }
    alertEl.textContent = "";
    status.textContent = msg;
    status.className = "fl-status" + (kind ? " fl-" + kind : "");
  };
  const progress = (frac) => { bar.hidden = false; fill.style.width = Math.round(frac * 100) + "%"; };
  // Both log phases read the board over the same wire — the difference is which
  // program on the chip is talking. Each phase opens with its own header so the
  // panel says which one you're looking at instead of leaving it to be inferred.
  const BOOT_HEAD = `— bootloader @ ${BOOT_BAUD} baud —\n`;
  const MON_HEAD = `— firmware @ ${MON_BAUD} baud —\n`;

  // esptool-js logs the chip/erase/write chatter through this sink — that's how
  // the board's details (type, revision, MAC, features) show after Connect. It
  // calls clean() itself on every main(), so the header is stamped there rather
  // than by the caller — otherwise esptool wipes it on the way in.
  const term = { clean() { logEl.textContent = BOOT_HEAD; }, writeLine: log, write: (d) => { details.hidden = false; logEl.textContent += d; } };

  const MAX_ATTEMPTS = 2;
  const CABLE_HELP =
    "Flashing failed — usually the USB cable or port. Use a short data cable, plug " +
    "straight into the computer (no hub or dock), and try again. Details in the log.";

  // The live connection held between the two clicks: null until Connect lands,
  // set back to null on a completed flash or any give-up. `image` stays null
  // while a variant pick is pending — the Flash button is disabled exactly then.
  let session = null;   // { port, transport, loader, chip, image, variants }
  let lastPort = null;  // most recent granted port — reused by the log monitor
  const setLabel = (label) => { go.textContent = label; };
  // The secondary button is an exit, never an entry: it exists only while
  // something is held — a board in the bootloader ("Disconnect") or a live log
  // stream ("Stop logs"). Idle has nothing to exit, so the button isn't there.
  // Raising it also arms it: startMonitor blocks on the read loop for the whole
  // stream, so the click handler's finally can't be what re-enables the way out.
  const setMon = (label) => { mon.textContent = label; mon.hidden = !label; mon.disabled = false; };

  // Variant picker (classic ESP32 only): rendered from the IMAGES entry, gates
  // the Flash button. Focus moves to the first radio — the pick IS the next step.
  function showPicker(variants) {
    pick.innerHTML = `<legend>Two boards use this chip — which is yours?</legend>` +
      variants.map((v, i) =>
        `<label><input type="radio" name="fl-board" value="${i}">
           <strong>${v.name}</strong><span class="fl-pick-hint">${v.hint}</span></label>`).join("");
    pick.hidden = false;
    for (const r of pick.querySelectorAll("input"))
      r.addEventListener("change", () => {
        const v = variants[+r.value];
        session.image = v;
        go.disabled = false;
        setLabel(`Flash ${v.name}`);
        say(`Ready — click Flash to write the ${v.name} firmware.`, "ok");
      });
    pick.querySelector("input").focus();
  }
  function hidePicker() { pick.hidden = true; pick.innerHTML = ""; }

  async function dropConnection() {
    if (session?.transport) { try { await session.transport.disconnect(); } catch {} }
    if (session) { session.transport = null; session.loader = null; }
  }
  async function endSession() { await dropConnection(); session = null; hidePicker(); }

  // Reuse a board we already have access to; only pop the browser picker when
  // there's nothing remembered or the choice is ambiguous. The first-ever grant
  // always prompts (the browser requires one explicit pick per origin); after
  // that getPorts() remembers it, even across reloads. With several remembered
  // ports we still prompt — silently flashing the wrong board beats a click.
  async function choosePort() {
    if (lastPort) return lastPort;
    const granted = await navigator.serial.getPorts();
    if (granted.length === 1) return granted[0];
    return navigator.serial.requestPort();   // 0 or >1 remembered → let the user pick
  }

  // Open the port, enter download mode, identify the chip. Throws on failure; a
  // `.notSupported` error must not be retried (re-writing the wrong chip fails
  // the same way). Leaves the loader live and ready to write.
  async function openBoard(port) {
    const transport = new Transport(port, true);
    const loader = new ESPLoader({ transport, baudrate: BOOT_BAUD, terminal: term });
    await loader.main();   // reset into download mode, upload stub, read chip info
    const chip = (loader.chip?.CHIP_NAME || "").toUpperCase();
    const entry = IMAGES[chip];
    if (!entry) {
      try { await transport.disconnect(); } catch {}
      const err = new Error(`Detected ${chip || "an unknown board"} — no firmware for it yet (supported: ${SUPPORTED}).`);
      err.notSupported = true;
      throw err;
    }
    // An ambiguous chip leaves `image` null until the user picks a variant.
    return { transport, loader, chip, image: entry.variants ? null : entry, variants: entry.variants || null };
  }

  async function writeImage(loader, image) {
    const fileArray = [];
    for (const [path, address] of image.parts)
      fileArray.push({ data: await fetchBinString(new URL(path, import.meta.url)), address });
    // Last, so a mid-flash failure leaves otadata untouched rather than pointing
    // at a slot this run didn't finish writing.
    fileArray.push({ data: "\xff".repeat(OTADATA_SIZE), address: OTADATA_ADDR });
    const totals = fileArray.map((f) => f.data.length);
    const grand = totals.reduce((a, b) => a + b, 0);
    say("Flashing — keep the cable connected…");
    await loader.writeFlash({
      fileArray, flashSize: "keep", flashMode: "keep", flashFreq: "keep",
      eraseAll: false, compress: true,
      reportProgress: (i, written) => {
        const done = totals.slice(0, i).reduce((a, b) => a + b, 0) + written;
        progress(done / grand);
      },
    });
    progress(1);
  }

  // Step 1 — connect and identify. The board sits in the bootloader afterward,
  // waiting for the Flash click; the log shows what it is.
  async function connect() {
    next.hidden = true;   // a new cycle invalidates the previous success guidance
    hidePicker();
    details.hidden = false; details.open = false; logEl.textContent = BOOT_HEAD;
    bar.hidden = true;
    let port;
    try { port = await choosePort(); }
    catch { say("No port selected — if the picker was empty, see “Board not showing up?” below.", "warn"); return; }
    lastPort = port;
    say("Connecting — hold BOOT if it doesn't start…");
    try {
      const s = await openBoard(port);
      session = { port, ...s };
      if (s.variants) {
        say(`Connected: ${s.chip} — two boards use this chip. Pick yours, then Flash.`, "ok");
        setLabel("Flash");   // stays disabled until a variant is picked
        showPicker(s.variants);
      } else {
        say(`Connected: ${s.chip}. Click Flash to write firmware, or Disconnect to release the board.`, "ok");
        setLabel(`Flash ${s.chip}`);
      }
      setMon("Disconnect");   // the board is held in the bootloader — logs are meaningless here
    } catch (e) {
      log("ERROR: " + (e?.message || e));
      say(e?.notSupported ? e.message
        : "Couldn't connect — hold BOOT while clicking Flash, or check the cable/port.", "err");
      await endSession();
      setLabel("Connect to flash");
    }
  }

  // Step 2 — write the connected board, retrying once on a transient write
  // failure. The first attempt uses the loader from Connect; a retry reopens the
  // port cleanly (esptool-js state after a failed write can't be reused).
  async function flash() {
    const { port, chip, image } = session;
    const board = image.name || chip;   // variant name when one was picked
    hidePicker();   // the choice is committed — the radios would only invite a mid-write change
    for (let attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
      try {
        if (!session.loader) {
          const s = await openBoard(port);
          session.transport = s.transport; session.loader = s.loader;
        }
        await writeImage(session.loader, image);
        say("Done — resetting the board…");
        await session.loader.after();   // hard-reset into the new firmware
        await endSession();
        setLabel("Connect to flash");
        // Success points at the criterion (a robot you can drive), not the
        // mechanism: the next-steps panel leads, and the boot log still streams
        // — collapsed — since this is the one moment logs are free (no wasted
        // bootloader detour). startMonitor re-pulses a reset so we capture from
        // the first boot line, reuses lastPort (no re-pick), and raises the
        // secondary button as "Stop logs".
        next.hidden = false;
        await new Promise((r) => setTimeout(r, 300));   // let the port settle after close before reopening
        await startMonitor(`✓ Flashed the ${board}.`);
        return;
      } catch (e) {
        log("ERROR: " + (e?.message || e));
        await dropConnection();          // failed loader is unusable — reopen next time
        if (attempt < MAX_ATTEMPTS) {
          say(`Write failed — retrying (attempt ${attempt + 1} of ${MAX_ATTEMPTS})…`, "warn");
          progress(0);
          await new Promise((r) => setTimeout(r, 800));   // let the port settle before reopening
        } else {
          bar.hidden = true;   // a stalled partial bar under the failure message reads as still-running
          say(CABLE_HELP, "err");
        }
      }
    }
    await endSession();
    setLabel("Connect to flash");
    setMon("");
  }

  // Release a held board without flashing: reset it into its app (so it isn't
  // left stranded in the download bootloader) and drop the connection.
  async function disconnect() {
    try { if (session?.loader) await session.loader.after(); } catch {}
    await endSession();
    setLabel("Connect to flash");
    setMon("");
    say("Disconnected — the board is running its firmware.");
  }

  // --- Serial monitor: stream the board's UART output at 115200 (the firmware's
  // monitor baud). Pure Web Serial — esptool only writes; reading is raw. Holds
  // the port for the duration, so it's mutually exclusive with flashing.
  let monitor = null;   // { port, reader } while streaming

  // Reached only from a completed flash — logs are the tail of that task, never
  // an errand of their own. `lead` prefixes the status line ("✓ Flashed…") so
  // the confirmation isn't lost when we roll straight into streaming.
  async function startMonitor(lead) {
    await endSession();   // release esptool's hold on the port, if any
    const port = lastPort;   // the board we just flashed — never re-pick
    try {
      await port.open({ baudRate: MON_BAUD });
    } catch {
      say(lead + " Couldn't show the boot log — something else is using the port. The board is running the new firmware.", "warn");
      return;
    }
    // Pulse a reset so boot output is captured. Safe at either wiring polarity:
    // we always end with both lines deasserted (reset released, runs the app).
    // Not all adapters support setSignals — a physical reset works too.
    try {
      await port.setSignals({ dataTerminalReady: false, requestToSend: true });
      await new Promise((r) => setTimeout(r, 120));
      await port.setSignals({ dataTerminalReady: false, requestToSend: false });
    } catch {}

    monitor = { port, reader: null };
    go.disabled = true;               // no flashing while the monitor owns the port
    setMon("Stop logs");
    // Collapsed: next steps lead, the boot log accumulates one keypress away.
    details.hidden = false; details.open = false;
    logEl.textContent = MON_HEAD;
    bar.hidden = true;
    say(lead + " Streaming logs — click Stop when done.", "ok");

    const dec = new TextDecoder();
    try {
      const reader = port.readable.getReader();
      monitor.reader = reader;
      // Accumulate off-DOM and flush once per frame — a per-chunk textContent
      // append re-copies the whole buffer (O(n²) on a chatty boot log).
      let buf = logEl.textContent, flushQueued = false;
      const flush = () => {
        flushQueued = false;
        const stick = atBottom();
        logEl.textContent = buf;
        if (stick) logEl.scrollTop = logEl.scrollHeight;
      };
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        if (!value) continue;
        buf += dec.decode(value, { stream: true });
        if (buf.length > 200000) buf = buf.slice(-120000);  // cap a chatty stream
        if (!flushQueued) { flushQueued = true; requestAnimationFrame(flush); }
      }
    } catch (e) {
      log("ERROR: " + (e?.message || e));
    } finally {
      await stopMonitor();
    }
  }

  async function stopMonitor() {
    if (!monitor) { setMon(""); go.disabled = false; return; }
    const { port, reader } = monitor;
    monitor = null;
    try { if (reader) { await reader.cancel(); reader.releaseLock(); } } catch {}
    try { await port.close(); } catch {}
    setMon("");
    go.disabled = false;
    say("Logs stopped.");
  }

  go.addEventListener("click", async () => {
    go.disabled = true; mon.disabled = true;
    try {
      if (session) await flash();
      else await connect();
    } finally {
      // A flash may have handed the port to the log monitor; a fresh connect may
      // be waiting on the variant pick (the radio's change handler re-enables).
      if (!monitor) go.disabled = !!(session && !session.image);
      mon.disabled = false;
    }
  });

  // Only ever visible in one of two held states, so it exits whichever holds.
  mon.addEventListener("click", async () => {
    if (monitor) { await stopMonitor(); return; }
    go.disabled = true; mon.disabled = true;   // held in the bootloader — release it
    try { await disconnect(); } finally { go.disabled = false; mon.disabled = false; }
  });
}
