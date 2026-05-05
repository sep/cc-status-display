---
layout: default
title: ClaudePanel — Display Firmware
---

# ClaudePanel — Display Firmware

**Flash the ESP32-S3 firmware that drives the ClaudePanel external status display.**

This is the *display firmware* piece of ClaudePanel. For the system overview
— what ClaudePanel is, how the plugin / bridge / firmware fit together, and
the install walkthrough that gets your Claude Code session talking to the
panel — start at the plugin repo:

→ **[sep/cc-status-plugin](https://github.com/sep/cc-status-plugin)**

If you've already installed the plugin and bridge and just need to put
firmware on a board: plug a supported ESP32-S3 into your computer with
USB-C, pick your board below, and click **Flash**.

## Supported boards

### SparkFun Thing Plus ESP32-S3

The original development board for this project. Compact, Qwiic connector,
microSD slot. HUB75 pins are split across both headers.

<p>
  <esp-web-install-button manifest="firmware/thing_plus/manifest.json">
    <button slot="activate" class="btn">Flash Thing Plus</button>
    <span slot="unsupported">Your browser doesn't support WebSerial. Try Chrome, Edge, or another Chromium-based browser.</span>
    <span slot="not-allowed">WebSerial requires HTTPS. Make sure you're on https://.</span>
  </esp-web-install-button>
  &nbsp;&nbsp;<a href="boards/thing_plus.html">Wiring &amp; pinout →</a>
</p>

### Lonely Binary ESP32-S3 N16R8 Gold Edition

16 MB Flash, 8 MB octal PSRAM. All HUB75 pins live on the left header,
GPIO 4–16 contiguous — a clean ribbon layout.

<p>
  <esp-web-install-button manifest="firmware/lonely_binary_n16r8/manifest.json">
    <button slot="activate" class="btn">Flash Lonely Binary</button>
    <span slot="unsupported">Your browser doesn't support WebSerial. Try Chrome, Edge, or another Chromium-based browser.</span>
    <span slot="not-allowed">WebSerial requires HTTPS. Make sure you're on https://.</span>
  </esp-web-install-button>
  &nbsp;&nbsp;<a href="boards/lonely_binary_n16r8.html">Wiring &amp; pinout →</a>
</p>

To add a new board, see
[`CONTRIBUTING.md`](https://github.com/sep/cc-status-display/blob/main/CONTRIBUTING.md).

## Browser support

Web flashing uses [WebSerial](https://developer.mozilla.org/en-US/docs/Web/API/Web_Serial_API),
which is supported in **Chrome, Edge, and other Chromium-based browsers**.
Firefox and Safari don't support WebSerial; for those browsers, use
[esptool](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/installation.html)
directly with the `.tar.gz` archives attached to each release.

## Repos

- This site / firmware: [sep/cc-status-display](https://github.com/sep/cc-status-display)
- Plugin (system entry point): [sep/cc-status-plugin](https://github.com/sep/cc-status-plugin)
- Bridge: [sep/cc-status-bridge](https://github.com/sep/cc-status-bridge)
- [Wire-protocol spec](https://github.com/sep/cc-status-bridge/blob/main/docs/FIRMWARE.md) (canonical, in the bridge repo)

<script
  type="module"
  src="https://cdn.jsdelivr.net/npm/esp-web-tools@10.2.1/dist/web/install-button.js?module"
></script>
