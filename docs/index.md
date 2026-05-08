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
</p>

Wiring & pinouts: [Thing Plus](boards/thing_plus.html) ·
[Lonely Binary](boards/lonely_binary_n16r8.html). New to ClaudePanel? Start
at the [plugin site](https://fluffy-adventure-o3kn381.pages.github.io/) —
that's the system overview and install walkthrough that gets your Claude
Code session talking to the panel. Don't have a board yet?
[Build your own](build-your-own.html) walks the parts list (~$50–80 total).
To add support for a new board, see
[CONTRIBUTING.md](https://github.com/sep/cc-status-display/blob/main/CONTRIBUTING.md).

## Reading the panel

Once your bridge is talking to a flashed board, the panel reports the state
of your Claude Code session in real time. A few behaviors worth knowing up
front:

- **`BLOCKED` strobes red.** The one state that demands your attention —
  strobes hard until acknowledged so you can't miss it from across the room.
- **The dev board's BOOT button silences the strobe.** Press once and
  BLOCKED drops into a calm steady glow. The state stays visible, you just
  stop being yelled at. Auto-clears the next time nothing on the panel is
  blocked, so the next blocked event re-alarms fresh.
- **Same button = AFK toggle when nothing's blocked.** Press it on your way
  out to lunch; the panel dims to ~10% with no animation, no strobing, no
  RX pulses — even if a `BLOCKED` arrives while you're gone, your neighbors
  get nothing but a faint glow. Press again on your way back to wake it.
  AFK persists across state changes, so Claude finishing a task while
  you're away won't light the panel up.
- **Auto-AFK after ~30 s of bridge silence.** Catches "Pause subscription"
  tray clicks, host sleep, and USB unplug with the same calm visual.
  Self-clears on the first byte from the bridge.
- **Screensaver after 5 minutes of quiet.** Same dim look as AFK,
  auto-engaged when every pinned slot has been `IDLE` or `BLOCKED` for the
  full timeout. Wakes on any state change or button press.
- **Bottom-left heartbeat pixel.** Green when bytes are flowing from the
  bridge (within the last 10 s); red when they aren't. Combined with
  auto-AFK you get a two-stage signal: red dot at 10 s ("nothing's
  flowing"), panel dim at 30 s ("you're fully away").

## Browser support

WebSerial works in Chrome, Edge, and other Chromium-based browsers — Firefox
and Safari don't. For those, use
[esptool](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/installation.html)
with the `.tar.gz` archives attached to each release.

## Related projects

- **[Plugin](https://fluffy-adventure-o3kn381.pages.github.io/)** —
  system entry point; install starts here.
- **[Bridge](https://miniature-dollop-g4kpjol.pages.github.io/)** —
  daemon that translates Claude Code events into wire frames for the
  firmware.
- **[Wire-protocol spec](https://github.com/sep/cc-status-bridge/blob/main/FIRMWARE.md)**
  — canonical, lives at the root of the bridge repo.

<script
  type="module"
  src="https://cdn.jsdelivr.net/npm/esp-web-tools@10.2.1/dist/web/install-button.js?module"
></script>
