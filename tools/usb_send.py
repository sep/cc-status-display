#!/usr/bin/env python3
"""
Send one NDJSON line to the display over USB and print what comes back.

    python tools/usb_send.py /dev/cu.usbmodem101 '{"type":"keygen"}'
    python tools/usb_send.py COM5 '{"type":"tunnel_status"}' --wait 10

Why not `screen` / `cat`: on the ESP32-S3's native USB port the DTR and RTS
lines drive reset and boot-mode selection (that's how esptool reboots the
chip). Generic terminal tools assert them on open, in an order the driver
chooses, and an out-of-phase RTS holds the chip in reset or drops it into
the ROM bootloader — silently. This script opens the port with both lines
held low, the way the bridge and esp-idf-monitor do.

Needs pyserial — present in the ESP-IDF python environment (`idf.py`'s
`python`), or `pip install pyserial`.
"""
import argparse
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial not found: run inside the ESP-IDF environment, or `pip install pyserial`")

ap = argparse.ArgumentParser()
ap.add_argument("port")
ap.add_argument("line", help="JSON object to send (sent with a trailing newline)")
ap.add_argument("--wait", type=float, default=5.0, help="seconds to keep printing replies (default 5)")
args = ap.parse_args()

with serial.Serial(args.port, 115200, timeout=0.2, dsrdtr=False, rtscts=False) as s:
    s.dtr = False
    s.rts = False
    s.reset_input_buffer()
    s.write(args.line.encode("utf-8") + b"\n")
    s.flush()
    deadline = time.time() + args.wait
    while time.time() < deadline:
        raw = s.readline()
        if not raw:
            continue
        text = raw.decode("utf-8", errors="replace").rstrip()
        # Device log lines share the port with the JSON replies; mark the JSON.
        print(("  >> " if text.startswith("{") else "     ") + text)
