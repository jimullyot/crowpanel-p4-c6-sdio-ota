#!/usr/bin/env python3
"""Reset the board and print its serial output for a while.

idf.py monitor needs a TTY, which an agent shell does not have. This does the
one thing monitor is needed for here -- pulse EN and read what comes back.
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.wchusbserial10"
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 45.0

with serial.Serial(port, 115200, timeout=0.5) as ser:
    # RTS drives EN on these boards; DTR must stay high or the chip enters
    # download mode instead of running the app.
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.15)
    ser.setRTS(False)

    deadline = time.time() + seconds
    while time.time() < deadline:
        chunk = ser.readline()
        if chunk:
            sys.stdout.write(chunk.decode("utf-8", "replace"))
            sys.stdout.flush()
