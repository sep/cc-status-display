# Contributing to claude-status

## Coding conventions

### Layout

Layout is enforced by [`.clang-format`](.clang-format). Run it before
committing:

```sh
clang-format -i $(git ls-files 'main/*.cpp' 'main/*.h')
```

Highlights of the configured style:

- 2-space indent, no tabs.
- 100-column limit.
- Pointer alignment to the right (`int *p`, not `int* p`).
- K&R-style braces, attached.
- Short forms allowed: one-line `if (x) y;` is fine; one-line empty blocks
  are fine; one-line non-empty blocks are not.
- Includes within a block are sorted alphabetically; blocks separated by a
  blank line are preserved (we use `"" group` then `<> group`).

### Naming

clang-tidy isn't enforcing names. The rules below are followed by hand:

| Kind                                | Style                | Example                                                   |
| ----------------------------------- | -------------------- | --------------------------------------------------------- |
| Functions                           | `snake_case`         | `apply_state_update`, `render_border`                     |
| Local variables                     | `snake_case`         | `panel_count`, `state_changed_at`                         |
| Globals                             | `g_snake_case`       | `g_mutex`, `g_panel_count`                                |
| Compile-time constants (any scope)  | `SCREAMING_SNAKE`    | `MAX_PANELS`, `FONT_W`, `IDENTIFY_COLOR`                  |
| Preprocessor macros                 | `SCREAMING_SNAKE`    | `BOARD_NAME`                                              |
| Types (struct, class, enum, alias)  | `PascalCase`         | `ClientSnapshot`, `Status`, `Viewport`, `Hub75Driver`     |
| Enum values                         | `SCREAMING_SNAKE`    | `Status::WORKING`, `SLOT_HALF_A`                          |
| Files                               | `snake_case.{cpp,h}` | `main.cpp`, `font5x7.h`, `board_config.h`                 |

Vendor APIs (Espressif, FreeRTOS, cJSON) use foreign conventions —
`usb_serial_jtag_*`, `xSemaphoreTake`, etc. Don't try to wrap or rename
them; just use them as-is.

### When in doubt

Match the surrounding code. The codebase is small enough that consistency
beats perfection.

## Adding a new board

1. Open [`main/board_config.h`](main/board_config.h).
2. Add a new `#define BOARD_NEW_NAME N` (next integer).
3. Add a new `#elif` block with a `BoardPins` struct and a `BOARD_LABEL`
   string. Include any board-specific pin quirks (RGB-channel swaps, OE
   GPIO, etc.) — keep them encapsulated here.
4. Update the CI matrix in
   [`.github/workflows/release.yml`](.github/workflows/release.yml) and the
   board-id list in [`scripts/make_manifest.py`](scripts/make_manifest.py).
5. Add a flash button entry in
   [`docs/site/index.html`](docs/site/index.html) and a wiring page under
   `docs/site/boards/` (copy an existing one as a template). Save the
   manufacturer's pinout image into `docs/site/boards/img/` with
   attribution in the page's `<figcaption>`.
6. Test-flash on the actual hardware. The firmware logs `board: <BOARD_LABEL>`
   at startup so you can confirm via UART monitor that the right preset is
   active.

## Adding a new state to the lexicon

The wire contract is in [`FIRMWARE.md` §4](FIRMWARE.md). Update the spec
first, then:

1. Add the new value to `enum class Status` in `main.cpp`.
2. Update `status_name()` and `parse_status_string()`.
3. Pick a color in `status_color_bright()` / `status_color_dim()`.
4. Pick a border animation in `border_intensity()`.
5. Add a long and short text label in `state_text_for()`.
6. Add the glyph(s) for any new letters to
   [`main/font5x7.h`](main/font5x7.h) if the new label needs them.

## Wire-protocol changes

Any change to the JSON shape goes in [`FIRMWARE.md`](FIRMWARE.md) **first**,
then implementation. The bridge author (separate repo) reads the spec to
understand what to send. Don't drift the implementation ahead of the spec.

## Commit & PR style

- Keep commits small and reviewable.
- Commit messages: imperative subject, body if non-obvious. "Why" matters
  more than "what" — the diff already shows what.
- Don't commit `sdkconfig` or anything under `build/`, `managed_components/`,
  `_site/`, `artifacts/` — they're in [`.gitignore`](.gitignore).
