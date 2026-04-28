#!/usr/bin/env python3
"""Generate an esp-web-tools manifest.json for one supported board.

Standard ESP32-S3 flash layout:
  0x0000   bootloader.bin
  0x8000   partition-table.bin
  0x10000  firmware.bin (app)

If the project's partition table changes such that the app offset shifts,
update the offset constants here to match the new flasher_args.json.
"""

import argparse
import json
import sys

BOARDS = {
    "thing_plus":          "SparkFun Thing Plus ESP32-S3",
    "lonely_binary_n16r8": "Lonely Binary ESP32-S3 N16R8 Gold Edition",
}

PARTS = [
    {"path": "bootloader.bin",       "offset": 0x0000},
    {"path": "partition-table.bin",  "offset": 0x8000},
    {"path": "firmware.bin",         "offset": 0x10000},
]


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--board-id", required=True, help=f"one of {list(BOARDS)}")
    p.add_argument("--version",  required=True, help="semver tag, e.g. v1.2.0")
    args = p.parse_args()

    if args.board_id not in BOARDS:
        sys.exit(f"unknown board id: {args.board_id}")

    manifest = {
        "name":    f"claude-status — {BOARDS[args.board_id]}",
        "version": args.version,
        "new_install_prompt_erase": True,
        "builds": [
            {
                "chipFamily": "ESP32-S3",
                "parts": PARTS,
            },
        ],
    }
    json.dump(manifest, sys.stdout, indent=2)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
