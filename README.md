# claude-status — display firmware

The ESP32-S3 firmware that renders Claude Code session state on a HUB75
RGB LED panel. **One of three components** in claude-status — the
plugin is the umbrella project with the full system overview, install
walkthrough, and pointers to the other pieces:

**→ <https://github.com/sep/cc-status-plugin>**

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

This repo is just the firmware: a small ESP32-S3 reads NDJSON state
updates over USB-Serial-JTAG and renders them on a 64×32 (or
chained-larger) HUB75 panel. The bridge hooks into the plugin's broker
and forwards state to the firmware over USB.

## Flash your hardware

If you've already installed the plugin + bridge and just need the
display half, the flasher site is the entry point:

**→ <https://sep.github.io/cc-status-display/>**

Pick your dev board and click Flash. Each board's page has its
GPIO-to-HUB75 pinout, power notes, and a verification recipe. Chrome
or Edge required (WebSerial).

Supported dev boards:

- **SparkFun Thing Plus ESP32-S3**
- **Lonely Binary ESP32-S3 N16R8 Gold Edition**

To add a new board, see [`CONTRIBUTING.md`](CONTRIBUTING.md).

## Developers

- **Wire-protocol spec** — authoritative for what bytes the bridge
  sends and the firmware accepts. Lives canonically in the bridge
  repo: <https://github.com/sep/cc-status-bridge/blob/main/docs/FIRMWARE.md>.
  Read it first if you're touching the serial path.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — coding conventions, how to add
  a new board, how to add a new state.
- [`MAINTENANCE.md`](MAINTENANCE.md) — every external dependency, how
  it's pinned, where to watch for upstream changes, and how to bump.
  Read this first when picking the project back up after months away.
- [`main/`](main/) — firmware source. Single-binary ESP-IDF v6.0
  project.

For the system architecture (how the plugin, bridge, and this firmware
fit together), see the [plugin repo's README][plugin].

[plugin]: https://github.com/sep/cc-status-plugin

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

CI builds per-board binaries, deploys the flasher site to GitHub Pages,
and attaches `.tar.gz` archives to a GitHub Release. Triggered by
pushing a semver-tagged commit:

```sh
git tag v0.1.0
git push --tags
```

See [`.github/workflows/release.yml`](.github/workflows/release.yml)
for the build matrix and steps.

## License

[MIT](LICENSE). © 2026 SEP.
