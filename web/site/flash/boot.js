// Page bootstrap — mounts the flasher into the landing page's #flasher slot.
// Kept as an external module (not an inline <script>) so the page's CSP can
// drop 'unsafe-inline' from script-src. flash.js resolves its own asset paths
// via import.meta.url, so this stays a two-liner regardless of hosting page.
import { mountFlasher } from "./flash.js";
mountFlasher(document.getElementById("flasher"));
