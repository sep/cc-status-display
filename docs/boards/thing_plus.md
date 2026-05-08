---
layout: default
title: SparkFun Thing Plus ESP32-S3 — ClaudePanel
---

[← All boards](../) · [← Plugin (system overview)](https://github.com/sep/cc-status-plugin)

# SparkFun Thing Plus ESP32-S3

**Wiring & flash instructions for ClaudePanel display firmware.**

## What you'll need

- SparkFun Thing Plus ESP32-S3 board
- WaveShare RGB-Matrix-P2.5-64×32 (or any HUB75 panel of compatible size)
- External 5 V power supply for the panel — 2 A peak rating, 4 A recommended
- Jumper wires (13 signal lines + at least one ground)
- USB-C cable to your computer
- **Recommended:** 10 kΩ resistor for OE pull-up (see below)
- **Optional:** a momentary push button if you want a dedicated "ack" control off-board — the BOOT button on the dev board works by default (see [The ack button](#the-ack-button))

## Wiring

### HUB75 → GPIO map

Wire each panel signal to the listed ESP32 GPIO. The firmware's pin
assignments for this board are baked in — don't substitute "conventional"
HUB75 pin orderings or colors will be wrong. Pin 8 (E) on the panel's IDC
is unused on a 1/16-scan panel; leave it disconnected.

| IDC pin # | Color / signal           | ESP32 GPIO    |
| --------- | ------------------------ | ------------- |
| 1         | R (top half)             | GPIO 1        |
| 2         | B (top half) ★           | GPIO 2        |
| 3         | G (top half) ★           | GPIO 4        |
| 4         | GND                      | **GPIO 42** ◆ |
| 5         | R (bottom half)          | GPIO 5        |
| 6         | B (bottom half) ★        | GPIO 6        |
| 7         | G (bottom half) ★        | GPIO 7        |
| 8         | E (NC on 1/16-scan)      | —             |
| 9         | A (row select)           | GPIO 10       |
| 10        | B (row select)           | GPIO 14       |
| 11        | C (row select)           | GPIO 15       |
| 12        | D (row select)           | GPIO 16       |
| 13        | CLK                      | GPIO 17       |
| 14        | LAT                      | GPIO 18       |
| 15        | OE                       | GPIO 21       |
| 16        | GND                      | board GND pin |

★ **Panel-quirk note:** on the WaveShare RGB-Matrix-P2.5-64×32 panel, IDC
pins 2 and 3 (and 6 and 7) are **swapped vs the HUB75 standard** — pin 2
internally drives the blue channel, pin 3 drives green. The firmware
compensates so that the natural sequential wiring above renders colors
correctly. If you substitute a different (HUB75-spec-compliant) panel,
edit `main/board_config.h` to swap `g1↔b1` and `g2↔b2` back to
conventional order.

◆ **Second-ground note:** the SparkFun Thing Plus only breaks out a
single GND pin to its headers, but HUB75 has two IDC ground pins (4 and
16) that both need a reference. Rather than asking you to solder a
jumper or scrounge a breakout strip, the firmware drives **GPIO 42** to
0 V at boot — wire IDC pin 4 to GPIO 42 the same way you'd wire any
other signal pin and you've got two grounds. The current through this
"GPIO ground" is signal-reference scale (a few mA) since the panel's
LED-current return rides its own 5 V supply, so it's well within the
ESP32-S3's 40 mA per-pin limit. Configured in `main/board_config.h` as
`BoardPins.aux_gnd`.

### Board pinout reference

<figure>
  <img src="img/sparkfun_thing_plus_esp32s3.jpg"
       alt="SparkFun Thing Plus ESP32-S3 header pinout">
  <figcaption>
    Image © SparkFun Electronics, used with attribution.
    <a href="https://github.com/sparkfun/SparkFun_Thing_Plus_ESP32-S3/blob/main/docs/hardware_overview.md">Source</a>.
  </figcaption>
</figure>

### Power

- Panel: external 5 V supply, ~2 A peak. **Don't try to power the panel
  from the ESP32's 5 V rail** — USB can't supply enough current.
- ESP32: USB-C from your computer.
- **Tie the panel's GND to one of the ESP32's GND pins.** Without a
  shared ground reference the data signals will be unstable even though
  the panel may appear to power up.

## Flash this board

Plug the ESP32-S3 into your computer with a USB-C cable, then click below.
Chrome, Edge, or another Chromium-based browser is required.

<p>
  <esp-web-install-button manifest="../firmware/thing_plus/manifest.json">
    <button slot="activate" class="btn">Flash Thing Plus</button>
    <span slot="unsupported">Your browser doesn't support WebSerial. Try Chrome, Edge, or another Chromium-based browser.</span>
    <span slot="not-allowed">WebSerial requires HTTPS. Make sure you're on https://.</span>
  </esp-web-install-button>
</p>

## Verify it works

1. Power the panel from your external 5 V supply.
2. After flashing finishes, the panel should show **"…"** in dim blue
   near the center, with a slow-breathing blue border. That's the
   firmware's "unknown" state — the chip is alive but no host has sent
   it any state yet.
3. Quick smoke test from a terminal (macOS):

   ```sh
   PORT=/dev/cu.usbmodem*    # tab-complete to the real path
   stty -f $PORT 115200 cs8 -cstopb -parenb raw
   echo '{"state":"working","subagent_count":2}' > $PORT
   ```

   Panel should switch to amber **WORKING** with a faster pulsing border
   and two amber bars near the bottom edge.

## The ack button

The **BOOT button** on your dev board does two different things depending
on whether anything is blocking when you press it.

### When something is blocked (the alarm case)

Press once to:

- Stop the strobing animation across every blocked slot driven by this ESP.
- Dim the BLOCKED text and indicators to a muted appearance.
- Keep the BLOCKED state visible (so you can walk away and still see "yes,
  still waiting on me"), but without the cortisol-spiking alarm.

The silence auto-clears once no slot on this ESP is blocked anymore, so
the next blocked event re-alarms fresh. Pressing the button multiple
times while still blocked is idempotent — frustration-smash safe. If you
press it while AFK was on (see below), the press also exits AFK so you
come back to a present-but-quiet panel in one click.

### When nothing is blocked (the "I'm walking away" case)

Press once to **toggle AFK mode**. AFK dims the entire panel to ~10%
brightness with no animation, no strobing, no RX pulses. If a BLOCKED
state arrives while AFK is on, it renders in the same calm dim style —
your neighbors don't get a flashing red light show while you're at lunch.
Press the button again on your way back to exit AFK.

AFK persists across state changes — the panel stays dim until you press
again, even if Claude finishes a long-running task while you're away.

### Auto-AFK on bridge silence

The panel also dims itself automatically when no bytes have arrived from
the bridge for 30 seconds — long enough to rule out a hiccup, short
enough to feel responsive. This catches "Pause subscription" tray clicks,
bridge crashes, host sleep, and USB unplug with the same calm visual.
Auto-AFK self-clears the moment any byte arrives again.

The bottom-left bridge-heartbeat pixel goes red 10 seconds into a silence
and stays red until bytes resume; auto-AFK kicks in 20 seconds after
that. Two stages, easy to read.

**Thing Plus caveat:** on this board, GPIO 0 drives both the BOOT button
and the onboard green status LED. The button still works (a press shorts
hard to ground), but the LED's current path can occasionally cause
false-positive ack triggers when idle. If you see this, wire a dedicated
button (see below) and set `BoardPins.ack_button = 42`.

### Optional: wire your own button

If you want a button somewhere more reachable than the dev board itself
(e.g., taped to your desk near your keyboard), wire a momentary push
button between any free GPIO and a GND pin. Then edit
`BoardPins.ack_button` in `main/board_config.h` to that GPIO number,
rebuild, and flash.

**No external pull-up resistor needed** — the firmware enables the
ESP32's internal pull-up (~45 kΩ).

- One button terminal → ESP GPIO (e.g., GPIO 42 — free on the left header)
- Other button terminal → any GND pin
- Active-low: not pressed reads HIGH, pressed reads LOW.

For long wire runs (more than ~30 cm) or electrically noisy environments,
an external 10 kΩ resistor between the GPIO and 3V3 adds noise immunity
but isn't required. Mechanical bounce is already handled in firmware (60
ms debounce), so a bypass capacitor is unnecessary.

## Optional: hardware OE pull-up

At chip reset, OE (GPIO 21) is briefly high-impedance before the firmware
takes over — this can cause a quick visual flash on the panel at boot. To
suppress it, solder a 10 kΩ resistor between OE and 3V3. The firmware's
early-OE-high block in `app_main()` trims the window further but a
hardware pull-up is the clean fix.

---

[← All boards](../) ·
[github.com/sep/cc-status-display](https://github.com/sep/cc-status-display) ·
[Wire-protocol spec](https://github.com/sep/cc-status-bridge/blob/main/docs/FIRMWARE.md)

<script
  type="module"
  src="https://cdn.jsdelivr.net/npm/esp-web-tools@10.2.1/dist/web/install-button.js?module"
></script>
