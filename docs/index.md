---
layout: default
title: ClaudePanel — Display Firmware
---

# ClaudePanel — Display Firmware

Flash the ESP32-S3 firmware that drives the ClaudePanel external status
display. Plug your board in via USB-C, pick it below, click **Flash**.

<p class="cta-row">
  <esp-web-install-button manifest="firmware/thing_plus/manifest.json">
    <button slot="activate" class="btn">Flash Thing Plus</button>
    <span slot="unsupported">WebSerial unsupported — use Chrome / Edge.</span>
    <span slot="not-allowed">WebSerial requires HTTPS.</span>
  </esp-web-install-button>
  <esp-web-install-button manifest="firmware/lonely_binary_n16r8/manifest.json">
    <button slot="activate" class="btn">Flash Lonely Binary</button>
    <span slot="unsupported">WebSerial unsupported — use Chrome / Edge.</span>
    <span slot="not-allowed">WebSerial requires HTTPS.</span>
  </esp-web-install-button>
  <a class="btn" href="build-your-own.html">Build your own →</a>
</p>

Wiring & pinouts: [Thing Plus](boards/thing_plus.html) ·
[Lonely Binary](boards/lonely_binary_n16r8.html). New to ClaudePanel? Start
at the [plugin repo](https://github.com/sep/cc-status-plugin) — it's the
system overview and the install walkthrough that gets your Claude Code
session talking to the panel. To add a new board, see
[CONTRIBUTING.md](https://github.com/sep/cc-status-display/blob/main/CONTRIBUTING.md).

## Browser support

WebSerial works in Chrome, Edge, and other Chromium-based browsers — Firefox
and Safari don't. For those, use
[esptool](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/installation.html)
with the `.tar.gz` archives attached to each release.

## Repos

- Firmware (this site): [sep/cc-status-display](https://github.com/sep/cc-status-display)
- Plugin: [sep/cc-status-plugin](https://github.com/sep/cc-status-plugin)
- Bridge: [sep/cc-status-bridge](https://github.com/sep/cc-status-bridge)
- [Wire-protocol spec](https://github.com/sep/cc-status-bridge/blob/main/docs/FIRMWARE.md) (lives in the bridge repo)

<script
  type="module"
  src="https://cdn.jsdelivr.net/npm/esp-web-tools@10.2.1/dist/web/install-button.js?module"
></script>
