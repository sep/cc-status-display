# TODO: Fritzing wiring diagrams

> Status: deferred. Per-board pages currently rely on the GPIO mapping
> table plus the manufacturer's photographic pinout image — both
> physically accurate. A proper hookup diagram (Fritzing-style) is
> high-value for non-technical users but takes real time to do
> faithfully, so it's parked here until someone has the bandwidth.

## Why this is a TODO and not a "won't do"

Per-board pages are good for someone who can read a pinout image and a
table. They're not great for someone who has never wired electronics
before — that audience needs a literal "this colored wire goes from
*here* to *here*" picture. Fritzing produces exactly that.

We tried a hand-rolled abstract SVG generator first and removed it: the
visual layout didn't reflect the boards' actual physical pin
arrangements (Thing Plus pins are split across two headers; Lonely
Binary's GPIOs aren't in numerical order on its left header), and a
diagram that's logically correct but physically misleading is worse
than no diagram. See
[`memory: hardware diagrams must reflect physical reality`](../.claude/projects/...)
for the principle going forward.

## Step-by-step

### 1. Install Fritzing

Download from <https://fritzing.org/download/>. Free, open-source,
macOS / Windows / Linux. Active maintenance.

### 2. Source the parts

Fritzing's strength is its parts library — diagrams look photoreal
because parts are vector renderings of real boards. We'd need three:

- **SparkFun Thing Plus ESP32-S3** — check
  <https://github.com/sparkfun/Fritzing_Parts>. Coverage of newer S3-era
  boards is spotty; community contributions on the Fritzing forum are
  often the actual source.
- **Lonely Binary ESP32-S3 N16R8 Gold Edition** — almost certainly not
  in any library. Smaller vendor.
- **HUB75 panel (or just the 16-pin IDC connector)** — community parts
  exist on GitHub (search "fritzing hub75"). The plain IDC connector
  alone is more likely to exist as a generic part.

For each missing part, options in increasing pain order:

1. **Substitute a similar board** (e.g., generic ESP32-S3 DevKitC) and
   relabel. Visually inaccurate, defeats the purpose — don't.
2. **Find a community part** on GitHub or the Fritzing forum. Drop the
   `.fzpz` file into Fritzing → `File → Import`.
3. **Build a custom part.** Genuinely substantial work — three SVG views
   (breadboard photo-realistic, schematic, PCB), connector definitions
   XML, metadata. Allow 2–4 hours per part for a first-timer. Adafruit
   has a good multi-part tutorial:
   <https://learn.adafruit.com/series/make-your-own-fritzing-parts>.

### 3. Build the diagram

Once parts are in the bin:

1. **New sketch** → switch to the **Breadboard view** (photo-realistic;
   the "Fritzing look").
2. **Drag** the dev board and the panel/IDC connector onto the canvas.
   Position them where they'll read well.
3. **Click a pin → drag to another pin** to draw a wire. Fritzing
   auto-routes; drag intermediate handles to clean up the route.
4. **Right-click any wire → "Wire color"**. Use these conventions to
   match the in-firmware/site styling:

   | Signal              | Color      |
   |---------------------|------------|
   | R1 / R2             | red        |
   | G1 / G2             | green      |
   | B1 / B2             | blue       |
   | A / B / C / D       | purple     |
   | CLK / LAT / OE      | amber/yellow |
   | GND                 | black      |

5. **Hover any pin** → tooltip shows its label. Confirms you're
   attaching to the right pin.
6. Iterate until it reads cleanly. Manual touch-ups beat auto-routing.

### 4. Export

`File → Export → as Image → SVG` (preferred — crisper, scales freely,
typically smaller than PNG for this complexity). Save into
`docs/boards/img/<board_id>_wiring.svg`.

### 5. Save the editable source

Save the `.fzz` (Fritzing's native project file) right next to the
exported SVG, e.g. `docs/boards/img/<board_id>_wiring.fzz`. That's
the source of truth — future tweaks edit the `.fzz` and re-export.

`.fzz` is a zipped-XML format; git handles it fine, but diffs aren't
human-readable. Treat it like a binary image asset.

### 6. Wire up to the board page

In `docs/boards/<board_id>.md`, add a new figure inside the
Wiring section (above the manufacturer pinout image):

```html
<figure>
  <img src="img/<board_id>_wiring.svg" alt="Hookup diagram: ...">
  <figcaption>
    Hookup diagram. Editable source:
    <a href="img/<board_id>_wiring.fzz">.fzz</a>.
  </figcaption>
</figure>
```

### 7. Update CONTRIBUTING.md

Add a step to the "Adding a new board" section: build a Fritzing
diagram and check in both `.fzz` and the exported `.svg`.

## Honest scope estimate

| Scenario                                     | Time per board |
|----------------------------------------------|----------------|
| All needed parts exist in the library        | 30–60 minutes  |
| Need to find community parts and import them | 1–2 hours      |
| Need to build a custom part from scratch     | 2–4 hours      |

The Thing Plus + HUB75 path is most likely to work with existing
community parts. The Lonely Binary will probably need a custom part.

## When you pick this up

A reasonable order of operations:

1. Scout: do parts exist for our two boards plus a HUB75 panel?
2. If yes, do the easier of the two boards first as a smoke test.
3. If no, decide whether to build the custom part or pick a different
   approach (an annotated photo of the actual hardware setup is a
   reasonable lower-effort alternative).
