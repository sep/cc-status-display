---
layout: default
title: Build Your Own — ClaudePanel
---

[← Display firmware home](./)

# Build Your Own

What you need to build a ClaudePanel display from scratch. Total
cost is around **$50–80 USD** depending on which dev board, which
panel, and how many of these things you already have on the bench.

Required parts first, then optional add-ons. Prices are approximate
USD as of late 2026 and will drift; check vendor pages for current
numbers.

## Required parts

| Item | Choice A | Choice B |
|---|---|---|
| **Dev board** | [SparkFun Thing Plus ESP32-S3](https://www.sparkfun.com/products/23172) — ~$25 | [Lonely Binary ESP32-S3 N16R8 Gold](https://lonelybinary.com/en-us/products/s3) — ~$20 |
| **HUB75 panel** | [WaveShare RGB-Matrix-P2.5-64×32](https://www.waveshare.com/rgb-matrix-p2.5-64x32.htm) — ~$30 | [Adafruit Product 5036](https://www.adafruit.com/product/5036) — ~$45 |
| **5 V power supply** | 5 V / 4 A center-positive barrel jack (any reputable brand) — ~$10 | — |
| **Jumper wires** | Female/female dupont kit, 14+ wires used — ~$5–10 | — |
| **USB-C cable** | Whatever you've got handy | $0 |

### Notes on the choices

- **Dev boards:** both work and the firmware tests against both.
  The Thing Plus is more polished and has a microSD slot if you
  want to extend, but its GPIO 0 has a known conflict with the
  onboard status LED that can cause flaky reads on the ack button
  (see the [Thing Plus wiring page](boards/thing_plus.html#the-ack-button)
  for the workaround). The Lonely Binary's GPIO 0 is clean, the
  module has more flash + PSRAM (16 MB / 8 MB), and it's cheaper —
  but it's a smaller vendor.
- **Panels:** both options above are the same OEM panel under
  different branding. Both have the [B/G-channel pin swap]
  (boards/thing_plus.html#wiring) that the firmware compensates
  for. Generic AliExpress P2.5-64×32 panels often follow standard
  HUB75 — they'll *mostly* look right but greens and blues will be
  swapped. Edit `BoardPins` in the firmware if you go that route.
- **Power supplies:** the panel can pull up to ~4 A at full
  brightness on all-white. **Don't try to power it from the
  ESP32's 5 V rail** — USB can't supply enough current and the
  panel will brown out the controller. An old phone charger is
  almost certainly *not* enough — read the spec sticker before
  you assume.
- **Jumper wires:** 13 signal lines + at least one ground = 14
  wires. Round up. Female/female is what you want for
  header-to-IDC connections.

## Optional add-ons

| Item | Why you'd want it | Cost |
|---|---|---|
| 10 kΩ resistor (¼ W) | Pull-up between OE and 3V3 — kills the brief boot-flash on the panel at chip reset. | ~$0.10 |
| 6×6 mm tactile push button + 2 jumpers | Off-board ack button if you'd rather not use the dev board's BOOT button (e.g., panel mounted across the room from the keyboard). | ~$0.30 |
| 16-pin IDC ribbon cable | Some panels ship with one; some don't. Check yours before ordering. | ~$3 |

These are all "from any starter electronics kit" parts. Mouser,
DigiKey, Amazon, AliExpress, your local hackerspace bin —
whatever's convenient.

## Already have some of this stuff?

Likely already on your bench:

- USB-C cable from any phone-era thing
- Jumper wires (most ESP32 starter kits include them)
- A barrel-jack 5 V supply that's *not* powerful enough for a P2.5
  panel — measure before assuming

## Next step

Once you've got the parts, head to your dev board's wiring page:

- [SparkFun Thing Plus ESP32-S3](boards/thing_plus.html)
- [Lonely Binary ESP32-S3 N16R8 Gold Edition](boards/lonely_binary_n16r8.html)

The flash button on each page is one click away once your board is
plugged in.

---

*Last reviewed: 2026-05-05.* Vendor pricing and SKU availability drift
over time; cross-check before ordering, especially if more than six
months have passed since this date.

[← Display firmware home](./) ·
[github.com/sep/cc-status-display](https://github.com/sep/cc-status-display)
