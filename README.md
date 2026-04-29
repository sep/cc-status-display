# claude-status

A HUB75 LED panel that shows what your Claude Code session is doing — live.

```
┌────────────────────────────────────┐
│  ▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒  │
│  ▒                              ▒  │
│  ▒        W O R K I N G         ▒  │
│  ▒                              ▒  │
│  ▒  ▮▮▮▮ ▮▮▮▮ ▮▮▮▮               ▒  │  ← 3 subagents running
│  ▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒  │
└────────────────────────────────────┘
```

A small ESP32-S3 reads NDJSON state updates over USB-Serial-JTAG and renders
them on a 64×32 (or chained-larger) HUB75 RGB LED panel. The host-side bridge
hooks into Claude Code's lifecycle events and forwards them to the panel.

## End users — flash a board

If you have one of the supported dev boards and a HUB75 panel, head to the
flasher site and click your board:

**→ https://sep.github.io/cc-status-display/**

Each board's page has a pinout diagram, wiring instructions, power notes, and
a one-click flash button (Chrome/Edge — WebSerial isn't yet supported in
Firefox or Safari).

Supported boards as of v1.2:

- **SparkFun Thing Plus ESP32-S3**
- **Lonely Binary ESP32-S3 N16R8 Gold Edition**

## Developers

- [`FIRMWARE.md`](FIRMWARE.md) — the wire-protocol spec, authoritative for
  what bytes the bridge sends and the firmware accepts. Read this first if
  you're touching the serial path.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — coding conventions, how to add a new
  board, how to add a new state.
- [`MAINTENANCE.md`](MAINTENANCE.md) — every external dependency, how it's
  pinned, where to watch for upstream changes, and how to bump. Read this
  first when picking the project back up after months away.
- [`main/`](main/) — firmware source. Single-binary ESP-IDF v6.0 project.

### Local build & flash

You'll need [ESP-IDF v6.0](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32s3/get-started/) installed, exported into your shell, and a supported board plugged in.

```sh
# Default build is for the SparkFun Thing Plus.
idf.py set-target esp32s3
idf.py build flash monitor

# To target a different board:
idf.py build -DBOARD_NAME=BOARD_LONELY_BINARY_N16R8_GOLD flash monitor
```

The `BOARD_NAME` macros are defined in [`main/board_config.h`](main/board_config.h).

### Cutting a release

CI builds per-board binaries, deploys the flasher site to GitHub Pages, and
attaches `.tar.gz` archives to a GitHub Release. Triggered by pushing a
semver-tagged commit:

```sh
git tag v1.2.0
git push --tags
```

See [`.github/workflows/release.yml`](.github/workflows/release.yml) for the
build matrix and steps.

## Architecture

The full system has three independent halves talking via NDJSON:

```
 Claude Code (WSL)
    │
    ▼ lifecycle hooks
 emit.py ─► broker.py        (localhost TCP, NDJSON pub/sub)
                │
                ▼
       ClaudeStatusBridge.exe (Windows-side, C# .NET)
                │
                ▼ NDJSON over USB-Serial-JTAG
        ESP32-S3 firmware    ← this repo
                │
                ▼ HUB75
        WaveShare RGB-Matrix-P2.5-64×32
```

Pointers to the other halves live in [`FIRMWARE.md` §11](FIRMWARE.md).

## License

[MIT](LICENSE). © 2026 SEP.
