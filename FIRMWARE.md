# ESP32 firmware contract — claude-status

This document is self-contained context for building the ESP32 firmware half
of the `claude-status` system. It is intended to be read in a fresh Claude
Code session on a development machine that has ESP-IDF v6.0 installed.

> **If you are a future Claude session reading this:** you do not need
> access to the original design conversation. Everything the firmware needs
> to know is captured below. The authoritative spec is the **Serial wire
> contract** section; other sections are context and suggestions.

---

## 1. System overview

```
 Claude Code (WSL)
    │
    ▼ lifecycle hooks (stdin JSON)
 emit.py  ─────►  broker.py                (localhost TCP, NDJSON, PUB/SUB)
                        │
                        ▼ SUB subscription
                   ClaudeStatusBridge.exe  (Windows-side, C# / .NET 10)
                        │
                        ▼ NDJSON over USB-Serial-JTAG @ 115200
                   ESP32-S3               ◄── YOU ARE HERE
                        │
                        ▼ HUB75
                   WaveShare RGB-Matrix-P2.5-64x32
```

The firmware's only job is to:

1. Read NDJSON lines from its USB-Serial-JTAG interface.
2. Maintain a "current state" in memory.
3. Render something visually meaningful on the 64×32 HUB75 matrix based on
   state (and any auxiliary fields, if desired).

The firmware **does not** need to speak back to the host. This is a
unidirectional feed for v1. If logging is needed, emit it on UART0 (the
dev-board's on-chip UART) rather than USB-CDC, so it doesn't interfere with
the incoming data channel.

---

## 2. Hardware assumptions

| Component      | Value                                                 |
|----------------|-------------------------------------------------------|
| MCU            | ESP32-S3 (native USB, USB-Serial-JTAG peripheral)     |
| Panel          | WaveShare RGB-Matrix-P2.5-64x32 (standard HUB75 IDC)  |
| Host transport | USB cable to Windows PC; shows up as a `COM` port     |
| Baud rate      | 115200 (cosmetic for USB-CDC; use it anyway for       |
|                | consistency with the bridge's configured value)       |

HUB75 pinout (standard 16-pin IDC):

```
R1 G1 B1 GND   R2 G2 B2 E
A  B  C  D     CLK LAT OE GND
```

Assign ESP32-S3 GPIOs to these signals per whatever HUB75 library is chosen
(see §6). The E pin is unused on a 32-row panel (1/16 scan) but must still
be connected to a safe GPIO.

---

## 3. Serial wire contract (authoritative)

Each message from host to device is **one UTF-8 JSON object**, terminated
by a single `\n` (0x0A).

Example (as bytes sent on the wire):

```
{"client":"1a","state":"working","event":"UserPromptSubmit","ts":1713648012.14}\n
```

### Fields

| Field             | Type    | Req? | Notes                                           |
|-------------------|---------|------|-------------------------------------------------|
| `state`           | string  | yes  | Lexicon in §4.                                  |
| `client`          | string  | no   | Target client slot (§3.1). Omitting defaults    |
|                   |         |      | to the firmware's lowest-ID full-panel slot,    |
|                   |         |      | which preserves single-panel v1 behavior.       |
| `subagent_count`  | integer | yes  | Number of concurrently-running Task subagents   |
|                   |         |      | in the pinned session. 0 means no subagents.    |
|                   |         |      | Bridge emits a fresh snapshot whenever this     |
|                   |         |      | value changes, even if `state` is unchanged —   |
|                   |         |      | so firmware should re-render on every line.     |
| `event`           | string  | no   | Original Claude Code hook event name, or        |
|                   |         |      | `"thinking-heuristic"` for bridge-derived       |
|                   |         |      | state. Informational.                           |
| `ts`              | number  | no   | Unix timestamp (seconds, float). Useful for     |
|                   |         |      | animations keyed on "time since last change."   |

**Forward compatibility:** the firmware MUST ignore unknown fields. Treat
each line independently; do not accumulate state across lines based on
"was this field present last time?"

**Malformed lines:** if a line fails to parse as JSON or lacks a valid
`state`, drop it silently. Do not crash, do not block subsequent lines.

**Rate:** expect bursts of a few lines per second during active Claude
sessions, with long idle periods in between. Buffer input to accommodate.

---

## 3.1 Addressing and multi-panel operation

A single firmware instance may drive one or more chained HUB75 panels as
a single logical framebuffer (e.g., two 64×32 panels chained → 128×32).
Each physical panel can render a **full-panel client** or be split
vertically into **two half-panel clients**.

### Client slot IDs

Client slot IDs are strings with the form `<N>[a|b]`:

- `N` (decimal integer, 1-indexed) — physical panel index. For a single
  firmware instance, panels are indexed from `first_id` to
  `first_id + panel_count - 1`, left-to-right in the chain.
- `a` / `b` suffix (optional) — half-panel selector. `a` is the left half
  (columns `0..31` within that panel), `b` is the right half (`32..63`).
- No suffix — the entire 64×32 panel.

Examples: `"1"`, `"1a"`, `"1b"`, `"2"`, `"2a"`, `"2b"`.

### Conflict rules (mutual exclusion per region)

Full-panel and half-panel slots on the same panel are **mutually
exclusive**, last-activity-wins per region:

- A line targeting `"1"` wipes any pinned `"1a"` and `"1b"` state; the
  full 64×32 area is redrawn from the `"1"` snapshot.
- A line targeting `"1a"` wipes any pinned `"1"` state; `"1a"` renders
  into its 32×32 half. If `"1b"` had no prior snapshot it renders as a
  dim "unpinned" placeholder (a single "·" glyph), so the user sees that
  the half is free to claim.
- A well-behaved bridge never mixes `"1"` and `"1a"`/`"1b"` streams in
  practice, but the firmware MUST tolerate the switch without crashing.

### Legacy / unaddressed lines

A line with no `client` field is interpreted as targeting the firmware's
lowest-ID full-panel slot — i.e., on a single-panel firmware with
`first_id = 1`, a bare `{"state":"working"}` behaves identically to
`{"client":"1","state":"working"}`. This preserves compatibility with
earlier single-panel scripts and test recipes.

### Unknown or out-of-range clients

Lines whose `client` references a slot this firmware doesn't own (e.g.,
`"3a"` when `panel_count=1`, `first_id=1`) are dropped silently. This
lets a single serial bus theoretically address multiple ESPs in the
future without each one crashing on foreign traffic — though v1.2
firmware is still 1-ESP-per-COM-port.

### Panel layout

Multi-panel setups use a compile-time default of `panel_count=1`,
`first_id=1`. The bridge can reconfigure these at runtime via the
`configure` command (§8), which is cached in NVS so reboots don't blank
the panel while waiting for the bridge to reconnect.

---

## 4. State lexicon (v1.2)

| `state` value  | Meaning                                                 |
|----------------|---------------------------------------------------------|
| `"working"`    | Claude received a prompt and is actively processing    |
|                | (typically tool calls in flight).                       |
| `"thinking"`   | Claude is in a working session but no tool activity    |
|                | has occurred for N seconds (bridge heuristic).          |
|                | Suggest same color family as `working`, different text  |
|                | or glyph (e.g. a meditation icon).                      |
| `"idle"`       | Claude has finished its turn; waiting for next prompt.  |
| `"blocked"`    | Claude is blocked on user input (permission prompt,     |
|                | notification requiring acknowledgment).                 |
| `"compacting"` | Claude Code is compressing the conversation context.    |
|                | Usually a 10–60s pause; worth a distinctive visual      |
|                | (e.g. a slow horizontal sweep) so the user doesn't      |
|                | think the system is frozen.                             |
| `"error"`      | A tool call returned an error. Warrants the loudest     |
|                | visual in your palette (bright red, flashing, etc.).    |

On startup, before any line is received, the firmware should render an
**implicit "unknown"** state (e.g. dim white, a "..." icon, or a connection
glyph) so the user knows the device is alive but not yet connected. Once
the first valid line arrives, switch to the reported state.

### State transition notes

- `thinking` is the bridge's best-guess derived state; it flips back to
  `working` the moment a tool call fires. Don't treat it as a terminal
  state — it's a decoration on top of `working`.
- `compacting` is always bracketed by `PreCompact` / `PostCompact` on
  the bridge side; after compaction the bridge restores whatever state
  was active beforehand (most often `working`).
- `error` is entered by a tool failure but is **not sticky** — the next
  lifecycle event (Stop, UserPromptSubmit, etc.) will transition out of
  it normally. Consider a brief "flash then hold" rendering so the error
  remains visible for a few seconds even if the host moves on quickly.

---

## 5. Suggested firmware structure (ESP-IDF v6.0)

Three FreeRTOS tasks, communicating through a shared state table
protected by a single mutex (contention is negligible at a few
lines/sec, so a single mutex for all client slots is fine).

```c
typedef enum {
    STATUS_UNKNOWN = 0,
    STATUS_IDLE,
    STATUS_WORKING,
    STATUS_THINKING,
    STATUS_BLOCKED,
    STATUS_COMPACTING,
    STATUS_ERROR,
} status_state_t;

typedef struct {
    status_state_t state;
    int64_t        changed_at_us;   // esp_timer_get_time() at last change
    int            subagent_count;  // 0..N concurrent Task subagents
    char           event[32];       // optional: last event name
    bool           pinned;          // false = slot unused / placeholder
} client_snapshot_t;

// Sized for panel_count * 3 slots: per panel, one full + two halves.
// MAX_PANELS is a compile-time cap (4 is generous for v1.2).
client_snapshot_t g_clients[MAX_PANELS * 3];
```

### Task 1 — Serial reader

- Install the USB-Serial-JTAG driver
  (`usb_serial_jtag_driver_install` / ESP-IDF's console component).
- In a loop: read bytes into a line buffer until `\n`, then parse.
- On parse success, update the shared snapshot and notify the render task
  (e.g. via an event group or simply by writing to the shared struct).

### Task 2 — Matrix render

- Drives the HUB75 panel at ~60–120 Hz refresh.
- Reads the shared snapshot each frame.
- Renders state-specific visuals. Suggestions (non-prescriptive):
  - `UNKNOWN`:  slow blue breathing, small "?" glyph
  - `IDLE`:     dim green dot, maybe a clock
  - `WORKING`:  yellow pulse, intensity keyed to `(now - changed_at)`
  - `BLOCKED`:  red with attention icon, maybe a slow blink
- Use `changed_at_us` to drive animations keyed to time-since-state-change
  rather than absolute time, so transitions feel responsive.

### Task 3 (optional) — Diagnostics

- Log to UART0 (the dev-board's non-USB serial) at some useful cadence,
  e.g. state changes or connection events. Never log to USB-CDC, as that
  channel is reserved for incoming host data.

---

## 6. HUB75 driver options for ESP-IDF

As of this writing, the most battle-tested HUB75 driver on ESP32-S3 is
**`mrfaptastic/ESP32-HUB75-MatrixPanel-DMA`**. It's primarily Arduino but
has ESP-IDF-compatible forks / components. Relevant considerations:

- Uses the S3's LCD_CAM peripheral (or I2S parallel on older chips) with
  DMA — frees the CPU for the render task.
- Configure panel dimensions: 64 width × 32 height, 1/16 scan.
- Color depth: typically 8-bit per channel after BCM, good enough for
  smooth animations on a 2.5mm panel.

Other paths a firmware author might explore (unverified, research
independently before committing):

- ESP-IDF native components: search the component registry at
  <https://components.espressif.com/> for "hub75" or "matrix".
- Pure ESP-IDF ports of `mrfaptastic` — several exist on GitHub; quality
  varies, inspect commit history.
- Rolling a custom driver on top of the `esp_lcd` peripheral: possible but
  significant effort; HUB75's row-select / OE latching is not a native
  LCD panel pattern.

Pin assignments should be captured in menuconfig or a `matrix_config.h`
header, not hard-coded, so they can be adjusted to match the physical
wiring.

---

## 7. Testing the firmware without Claude

You do not need to run Claude Code to exercise the firmware. You can
send lines directly into the COM port:

### From a Windows PowerShell (with the device on COM4):

```powershell
$p = [System.IO.Ports.SerialPort]::new('COM4', 115200, 'None', 8, 'One')
$p.NewLine = "`n"
$p.Open()
$p.WriteLine('{"state":"working","event":"test","ts":0}')
Start-Sleep -Seconds 2
$p.WriteLine('{"state":"blocked","event":"test","ts":0}')
Start-Sleep -Seconds 2
$p.WriteLine('{"state":"idle","event":"test","ts":0}')
$p.Close()
```

### From Linux (after the device enumerates as `/dev/ttyACM0` or similar):

```bash
stty -F /dev/ttyACM0 115200 cs8 -cstopb -parenb raw
# v1.2-addressed (explicit client)
echo '{"client":"1","state":"working","subagent_count":0}' > /dev/ttyACM0
sleep 2
echo '{"client":"1","state":"working","subagent_count":3}' > /dev/ttyACM0
sleep 2
echo '{"client":"1","state":"blocked","subagent_count":3}' > /dev/ttyACM0
sleep 2
echo '{"client":"1","state":"idle","subagent_count":0}'    > /dev/ttyACM0

# Split-panel demo — two clients on halves of panel 1
echo '{"client":"1a","state":"working","subagent_count":2}' > /dev/ttyACM0
echo '{"client":"1b","state":"thinking","subagent_count":0}' > /dev/ttyACM0
```

### Soak test — random state + count transitions, one client:

```bash
while true; do
  s=$(shuf -n1 -e idle working thinking blocked compacting error)
  c=$(shuf -n1 -e 0 0 0 1 2 3 5)   # weighted toward 0
  echo "{\"client\":\"1\",\"state\":\"$s\",\"subagent_count\":$c,\"ts\":$(date +%s.%N)}" \
    > /dev/ttyACM0
  sleep 1
done
```

### Soak test — two-client split, independent transitions:

```bash
while true; do
  for slot in 1a 1b; do
    s=$(shuf -n1 -e idle working thinking blocked compacting error)
    c=$(shuf -n1 -e 0 0 0 1 2 3 5)
    echo "{\"client\":\"$slot\",\"state\":\"$s\",\"subagent_count\":$c}" > /dev/ttyACM0
  done
  sleep 1
done
```

These are the fastest iteration loops for firmware work: no Claude, no
bridge, no broker — just hand-crafted NDJSON into the serial port. The
single-client soak covers all six states and a plausible range of
subagent counts; the split-panel soak additionally shakes out per-half
rendering, mutex contention, and the "both halves active at once"
layout.

---

## 8. Bidirectional protocol extension (v1.2)

The v1.0 contract (§3) was unidirectional host→device. v1.1 added
PING/PONG and RESYNC for liveness observability. v1.2 adds runtime
panel configuration and a user-facing identify command, and generalizes
the PONG shape to report multiple client slots.

All of this is **additive** — a v1.0 firmware that ignores unknown
fields remains functional, it just won't answer pings, can't be
reconfigured, and only addresses the default single-panel slot.

### Dispatch rule (device side)

On every incoming line, parse JSON then dispatch by `type`:

| `type` value        | Action                                                    |
|---------------------|-----------------------------------------------------------|
| `"state"`           | Treat `state` / `client` / `event` / `ts` / etc. as a    |
|                     | state update for the addressed client (§3 + §3.1).        |
| `"ping"`            | Respond with a `pong` (see below).                        |
| `"resync"`          | Respond with a spontaneous state for every pinned client. |
| `"identify"`        | Briefly render this ESP's panel IDs and resume (see       |
|                     | below).                                                   |
| `"configure"`       | Apply new panel layout at runtime, ack with `configured`. |
| *(missing)*         | Legacy fallback: if a `state` field is present, treat as  |
|                     | a v1 state update targeting the default client. Otherwise |
|                     | ignore the line.                                          |
| *(anything else)*   | Ignore. Forward-compat for future types.                  |

### Host → device messages

**PING** — periodic liveness probe, sent by bridge:

```json
{"type":"ping","seq":1234,"ts":1713648012.14}
```

Bridge sends pings every N seconds (default 5s). Each ping carries a
monotonically increasing `seq` so host can pair responses.

**RESYNC** — host asks device to report its current rendering state:

```json
{"type":"resync"}
```

Useful on bridge startup to detect if the device is already displaying
something that differs from the host's view of "current state."

**IDENTIFY** — host asks each ESP to briefly render its panel IDs
large and centered, so the user can verify which physical display is
which after rearranging cables:

```json
{"type":"identify","duration_ms":5000}
```

- `duration_ms` — how long to show the ID. Optional; defaults to `5000`.
  Firmware MUST clamp to `[500, 30000]` so a typo can't wedge the
  display.
- During the identify window the firmware suspends normal state
  rendering and paints each owned panel's ID (e.g., `"1"`, or `"1 2"`
  for a chain) in large glyphs. State updates received during the
  window are applied to the snapshot but not rendered until identify
  ends.
- Bridge typically emits identify only on explicit user request.

**CONFIGURE** — host reconfigures the panel layout at runtime:

```json
{"type":"configure","panel_count":1,"panel_width":64,"panel_height":32,
 "layout":"horizontal","first_id":1}
```

- `panel_count` (1..4) — chained panels driven by this ESP.
- `panel_width` — must be in the allow-list `{32, 64}`.
- `panel_height` — must be in the allow-list `{16, 32, 64}`.
- `layout` — one of `"horizontal"`, `"vertical"`, `"serpentine"`. Only
  `"horizontal"` is fully exercised in v1.2.
- `first_id` (1..99) — the lowest client ID this ESP owns; the others
  are `first_id..first_id+panel_count-1`.
- **Validation is strict**: any missing / out-of-range / wrong-type
  field causes the entire command to be dropped silently. The firmware
  never partially applies a configure.
- **No-op short-circuit**: if the new config is byte-identical to the
  current config, firmware skips the driver teardown/rebuild entirely
  to avoid DMA churn. A `configured` ack is still emitted.
- **NVS-backed**: the last successfully-applied configure is cached in
  NVS so reboots restore it, preventing a blank panel while waiting for
  the bridge to reconnect.
- **Pins are NOT configurable**: GPIO assignments are physical wiring
  and live in firmware. The bridge never specifies pins.

### Device → host messages

**PONG** — response to a ping. Reports a summary of every currently-
pinned client slot:

```json
{
  "type":"pong","seq":1234,
  "panel_count":1,"first_id":1,
  "clients":{
    "1a":{"state":"working","subagent_count":2,"uptime_ms_at_change":121000},
    "1b":{"state":"idle","subagent_count":0,"uptime_ms_at_change":98500}
  },
  "uptime_ms":123456
}
```

- `seq` MUST echo the ping's `seq` verbatim.
- `clients` — map of client-ID → per-slot snapshot. Only slots that
  have ever received a state update appear here; unpinned slots are
  omitted.
- `panel_count` / `first_id` — echo the current layout for bridge
  sanity-checking.
- `uptime_ms` — device's `esp_timer_get_time() / 1000`, useful for
  detecting device reboots across pings.
- `uptime_ms_at_change` — value of `uptime_ms` when that slot's state
  last changed, for the bridge's "time since last change" display.

**Spontaneous state** (response to `resync`, or proactive notification):

```json
{"type":"state","client":"1a","state":"working","subagent_count":2,
 "uptime_ms":123456}
```

- One message per pinned client. On `resync`, firmware emits N of these
  back-to-back (one per populated slot).
- Also sent when the device's rendering state changes for reasons other
  than a direct host instruction (e.g., a local reboot, or transitioning
  a slot to "unknown" after N seconds without contact for that slot).

**CONFIGURED** — ack for a successful configure:

```json
{"type":"configured","panel_count":1,"panel_width":64,"panel_height":32,
 "layout":"horizontal","first_id":1,"changed":true,"uptime_ms":123456}
```

- Echoes the applied config verbatim (including the no-op case, where
  `changed:false` signals "already this way, nothing to do").
- Bridge should not assume a configure succeeded until it sees the
  matching `configured` ack; silent drops (validation failures) produce
  no response.

### Bridge behavior

- Send PING every `PingIntervalMs` (default: 5000).
- If no PONG within `PingTimeoutMs` (default: 2000) after the most
  recent PING, log `[bridge] device unresponsive (N missed pongs)` and
  continue. Do NOT stop forwarding state updates when pongs are
  missing — USB-CDC writes still happen. PING/PONG is informational.
- On connect / reconnect, the bridge SHOULD emit a `configure` with
  its intended layout followed by a `resync` so the initial state is
  reconciled before real traffic starts.
- Bridge MAY publish device status to the broker later (as a separate
  metric) if we decide to surface device-offline state inside Claude.

### Firmware test recipe for bidirectional traffic

```bash
# Watch the device's responses on the same serial port:
stty -F /dev/ttyACM0 115200 cs8 -cstopb -parenb raw
# Terminal A:
cat /dev/ttyACM0
# Terminal B:
echo '{"type":"ping","seq":1}'                                 > /dev/ttyACM0
# Expect: {"type":"pong","seq":1,"panel_count":...,"clients":{...}}

echo '{"type":"identify"}'                                     > /dev/ttyACM0
# Expect: 5s of large-glyph panel IDs on the display, then resume.

echo '{"type":"configure","panel_count":2,"panel_width":64,"panel_height":32,"layout":"horizontal","first_id":1}' > /dev/ttyACM0
# Expect: {"type":"configured","panel_count":2,...,"changed":true,...}

echo '{"type":"configure","panel_count":9999}'                 > /dev/ttyACM0
# Expect: no response (validation drop); display unchanged.
```

---

## 9. Known extension points (for later)

When adding firmware support for future fields the bridge may emit,
prefer a data-driven approach over hard-coding. Likely future fields:

- `tool_name` (string) — currently-executing tool. Rendering: small text
  banner, or a themed icon.
- `progress` (object) — when Claude has emitted a multi-step plan, the
  bridge may include `{"current":2,"total":7}` so the firmware can draw
  a progress bar along the top border.
- `elapsed_ms` (int) — time since current state started. The firmware
  can already compute this locally from `ts` + `esp_timer_get_time`, so
  this field is only useful if the host has more precise info.

Likely future `type`-dispatched commands:

- `"brightness"` — adjust panel brightness at runtime without flashing
  new firmware. Shape: `{"type":"brightness","level":0..255}`.
- `"set_first_id"` — reassign this ESP's `first_id` without touching
  the rest of the configure payload. Convenience alias when the bridge
  just wants to renumber.

---

## 10. Getting unstuck

- **Device not enumerating as a COM port on Windows:** check that the
  ESP32-S3 is in normal run mode (not bootloader); the native USB CDC
  should auto-install on Windows 10+.
- **Seeing garbled bytes on the serial channel:** confirm UART logging is
  routed to UART0, not USB-CDC; check `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`
  vs `CONFIG_ESP_CONSOLE_UART_DEFAULT` in menuconfig.
- **Lines appearing truncated:** increase the input buffer on the serial
  reader task; bursts from the bridge can be multiple lines deep.
- **Matrix flickers:** the HUB75 render task must have CPU headroom. If
  the serial reader is busy-looping or doing heavy JSON work, starvation
  can cause visual tearing. Keep the reader's per-loop work minimal.

---

## 11. Pointers to the other halves

- **Plugin** (Claude Code hooks + broker, Python):
  <https://github.com/sep/cc-status-plugin>. This is the umbrella project
  for the whole claude-status system; start here for the system-level
  overview. The relevant source files are `bin/broker.py` (TCP NDJSON
  broker) and `bin/emit.py` (hook publisher).
- **Bridge** (Windows-side transport, C# / .NET): the sibling
  `cc-status-bridge` repo under the same org. See `SerialOutput.cs`
  and `StateMapper.cs` for what the bridge emits onto the serial port
  — this is the authoritative upstream for the wire contract in §3.
- The bridge mirrors broker discovery state to
  `%USERPROFILE%\.claude-status\sessions\<id>\broker.json` on the
  Windows side, so the Windows EXE can find live sessions without
  traversing `\\wsl$\`. Firmware does not need to know about this — it
  only sees the serial output.
