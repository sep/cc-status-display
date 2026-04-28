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
struct BoardPins {
  int8_t r1, g1, b1;
  int8_t r2, g2, b2;
  int8_t a, b, c, d, e;
  int8_t clk, lat, oe;
};

// ============================================================================
// SparkFun Thing Plus ESP32-S3
// https://www.sparkfun.com/products/23172
// ============================================================================
#if BOARD_NAME == BOARD_SPARKFUN_THING_PLUS_ESP32_S3

constexpr BoardPins BOARD_PINS = {
    // G1/B1 and G2/B2 are deliberately swapped vs. the conventional HUB75
    // ordering — this matches the as-built ribbon wiring on the dev board
    // this project was bring-up'd on. Don't "normalize" without rewiring.
    .r1 = 1,  .g1 = 4,  .b1 = 2,
    .r2 = 5,  .g2 = 7,  .b2 = 6,
    .a  = 10, .b  = 14, .c  = 15, .d = 16, .e = -1,
    .clk = 17, .lat = 18, .oe = 21,
};
constexpr const char *BOARD_LABEL = "SparkFun Thing Plus ESP32-S3";

// ============================================================================
// Lonely Binary ESP32-S3 N16R8 Gold Edition
// https://lonelybinary.com/en-us/products/s3
// All HUB75 pins live on the left header, GPIO 4..16 contiguously.
// Conventional R/G/B order — wiring is fresh, no inherited swap.
// ============================================================================
#elif BOARD_NAME == BOARD_LONELY_BINARY_N16R8_GOLD

constexpr BoardPins BOARD_PINS = {
    .r1 = 4,  .g1 = 5,  .b1 = 6,
    .r2 = 7,  .g2 = 8,  .b2 = 9,
    .a  = 10, .b  = 11, .c  = 12, .d = 13, .e = -1,
    .clk = 14, .lat = 15, .oe = 16,
};
constexpr const char *BOARD_LABEL = "Lonely Binary ESP32-S3 N16R8 Gold Edition";

#else
#error "Unknown BOARD_NAME — add a preset to board_config.h or pick an existing one"
#endif
