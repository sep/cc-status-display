#pragma once

#include <stdint.h>

// ============================================================================
// Board selection
// ============================================================================
// Pick exactly one. Default is the SparkFun Thing Plus ESP32-S3, which is
// the board this firmware was bring-up'd on. To target a different board,
// either change BOARD_NAME below or pass it on the build command line:
//
//     idf.py build -DBOARD_NAME=BOARD_LONELY_BINARY_N16R8_GOLD
//
// Adding a new board: append a new #elif block at the bottom with that
// board's pin map and any board-specific quirks (e.g., color-channel swaps
// to match physical ribbon wiring). Don't add per-board logic outside this
// file — keep all board-specific knowledge here so a new board is one PR.
// ============================================================================

#define BOARD_SPARKFUN_THING_PLUS_ESP32_S3 1
#define BOARD_LONELY_BINARY_N16R8_GOLD     2

#ifndef BOARD_NAME
#define BOARD_NAME BOARD_SPARKFUN_THING_PLUS_ESP32_S3
#endif

// HUB75 GPIO assignment for one ESP32 driving up to MAX_PANELS chained
// panels. `e` is -1 for 1/16-scan panels (32 rows or fewer); set it to a
// real GPIO for 1/32-scan (64-row) panels.
//
// `ack_button` is the GPIO of a momentary push button (active-low, with
// an internal pull-up). Pressing it silences the BLOCKED-state strobe
// across every slot driven by this ESP. Default of 0 reuses the BOOT
// button on most ESP32-S3 dev boards (no extra hardware needed). Set to
// -1 to disable the feature entirely.
struct BoardPins {
  int8_t r1, g1, b1;
  int8_t r2, g2, b2;
  int8_t a, b, c, d, e;
  int8_t clk, lat, oe;
  int8_t ack_button;
};

// ============================================================================
// Panel-specific note: G/B swap vs HUB75 spec
// ----------------------------------------------------------------------------
// All currently-supported boards have G1/B1 (and G2/B2) GPIO assignments
// swapped vs the literal HUB75 standard. This is NOT a board quirk — it
// compensates for the WaveShare RGB-Matrix-P2.5-64x32 panel's non-standard
// pinout: that panel's IDC pin 2 internally drives the blue channel (where
// the spec says G1 lives), and pin 3 drives the green channel. Same for
// pins 6/7 on the bottom half.
//
// With this firmware swap, a sequential wiring (panel pin N → ESP GPIO N
// in the natural ascending order) renders colors correctly. If you ever
// substitute a HUB75-spec-compliant panel, swap these back: g1↔b1, g2↔b2.
// ============================================================================

// ============================================================================
// SparkFun Thing Plus ESP32-S3
// https://www.sparkfun.com/products/23172
// ============================================================================
#if BOARD_NAME == BOARD_SPARKFUN_THING_PLUS_ESP32_S3

constexpr BoardPins BOARD_PINS = {
    .r1 = 1,  .g1 = 4,  .b1 = 2,
    .r2 = 5,  .g2 = 7,  .b2 = 6,
    .a  = 10, .b  = 14, .c  = 15, .d = 16, .e = -1,
    .clk = 17, .lat = 18, .oe = 21,
    // GPIO 0 is the BOOT button AND the onboard green status LED on this
    // board. The button still works as input (the press shorts hard to
    // ground), but the LED's current path may make idle reads noisy. If
    // you see false-positive ack triggers, set ack_button = 42 and wire
    // a momentary switch from GPIO 42 to GND on the left header.
    .ack_button = 0,
};
constexpr const char *BOARD_LABEL = "SparkFun Thing Plus ESP32-S3";

// ============================================================================
// Lonely Binary ESP32-S3 N16R8 Gold Edition
// https://lonelybinary.com/en-us/products/s3
// All HUB75 pins live on the left header, GPIO 4..16 contiguously.
// G1↔B1 and G2↔B2 are swapped to compensate for the WaveShare panel — see
// the panel-specific note above this block.
// ============================================================================
#elif BOARD_NAME == BOARD_LONELY_BINARY_N16R8_GOLD

constexpr BoardPins BOARD_PINS = {
    .r1 = 4,  .g1 = 6,  .b1 = 5,
    .r2 = 7,  .g2 = 9,  .b2 = 8,
    .a  = 10, .b  = 11, .c  = 12, .d = 13, .e = -1,
    .clk = 14, .lat = 15, .oe = 16,
    .ack_button = 0,  // BOOT button. Clean GPIO 0 (no LED conflict).
};
constexpr const char *BOARD_LABEL = "Lonely Binary ESP32-S3 N16R8 Gold Edition";

#else
#error "Unknown BOARD_NAME — add a preset to board_config.h or pick an existing one"
#endif
