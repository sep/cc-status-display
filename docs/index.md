---
layout: default
title: ClaudePanel — Display Firmware
---

# ClaudePanel — Display Firmware

> **Not sure where to start?** Begin at the
> [plugin site](https://sep.github.io/cc-status-plugin/) — that's
> the system overview and install walkthrough that gets your Claude Code
> session talking to the panel.

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

- **Wiring & pinouts:** [Thing Plus](boards/thing_plus.html) ·
  [Lonely Binary](boards/lonely_binary_n16r8.html)
- **[Build your own](build-your-own.html)** — parts list, ~$50–80 total.
- **[Adding a new board](https://github.com/sep/cc-status-display/blob/main/CONTRIBUTING.md)**
  (CONTRIBUTING.md)

## Reading the panel

Once your bridge is talking to a flashed board, the panel reports the state
of your Claude Code session in real time. A few behaviors worth knowing up
front:

- **`BLOCKED` strobes red.** The one state that demands your attention —
  strobes hard until acknowledged so you can't miss it from across the room.
- **The dev board's BOOT button is a single context-sensitive ack.** What
  it does depends on what's loudest at the moment of the press:
    1. **Loud `BLOCKED` strobe?** First press silences it — BLOCKED drops
       into a calm steady glow. The state stays visible, you just stop
       being yelled at. Auto-clears the next time nothing on the panel is
       blocked, so the next blocked event re-alarms fresh.
    2. **Already silenced, or nothing blocked?** The press toggles AFK
       instead. So "I see it, going to lunch" is two presses: silence,
       then AFK. The panel dims to ~10% with no animation, no strobing,
       no RX pulses — and a large blue `AFK` is composed over the
       middle of each panel as a "you parked this, press to wake"
       reminder. Even if a fresh `BLOCKED` arrives while you're gone,
       your neighbors get nothing but a faint glow.
    3. **Coming back?** Press once to exit AFK. If a block is still
       active, it's auto-silenced on wake so the panel doesn't start
       strobing the moment you sit down.
  AFK persists across state changes — Claude finishing a task while
  you're away won't light the panel up.
- **Auto-AFK after ~30 s of bridge silence.** Catches "Pause subscription"
  tray clicks, host sleep, and USB unplug with the same calm visual.
  Self-clears on the first byte from the bridge.
- **Screensaver after 5 minutes of quiet.** Same dim look as AFK but
  *without* the blue `AFK` overlay — that's the visual triage cue. If
  you see big blue letters, you have to press the button to wake; if
  it's just dim, any new state update will wake it. Screensaver
  auto-engages when every pinned slot has been `IDLE` or `BLOCKED` for
  the full timeout.
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

- **[Plugin](https://sep.github.io/cc-status-plugin/)** —
  system entry point; install starts here.
- **[Bridge](https://sep.github.io/cc-status-bridge/)** —
  daemon that translates Claude Code events into wire frames for the
  firmware.
- **[Wire-protocol spec](https://github.com/sep/cc-status-bridge/blob/main/FIRMWARE.md)**
  — canonical, lives at the root of the bridge repo.

<script
  type="module"
  src="https://cdn.jsdelivr.net/npm/esp-web-tools@10.2.1/dist/web/install-button.js?module"
></script>
