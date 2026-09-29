#!/usr/bin/env python3
"""monitor-com.py — stream the llm-tick app's CDC console (COM5) with DTR asserted.

The app's TinyUSB CDC (ARDUINO_USB_MODE=0 composite: console + HID keyboard)
only streams when DTR is asserted. This script opens the port, sets DTR, and
prints every byte it receives, timestamped.

Usage:
  python tools/monitor-com.py [PORT] [BAUD] [--timeout SECONDS]

  PORT   default COM5 (the TinyUSB CDC console).
  BAUD   default 115200.
  --timeout N   auto-exit after N seconds (default: run until Ctrl+C).

Exits cleanly on Ctrl+C. Useful for watching [ble] state logs while pairing the
Keychron (Fn+B1), and for passing serial commands like "TYPE Hi" in another
terminal.
"""
import argparse
import sys
import time

import serial

def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("port", nargs="?", default="COM5")
    p.add_argument("baud", nargs="?", type=int, default=115200)
    p.add_argument("--timeout", type=float, default=None,
                   help="exit after N seconds instead of running until Ctrl+C")
    a = p.parse_args()

    try:
        s = serial.Serial(port=a.port, baudrate=a.baud, timeout=0.05,
                          rtscts=False, dsrdtr=False)
    except serial.SerialException as e:
        print(f"[monitor] cannot open {a.port}: {e}")
        return 1

    # Assert DTR so the app's CDC starts streaming.
    s.dtr = True
    print(f"[monitor] COM5-like console open on {a.port} @ {a.baud}, DTR set. Ctrl+C to stop.")
    s.reset_input_buffer()

    deadline = time.time() + a.timeout if a.timeout else None
    try:
        while True:
            try:
                data = s.read(512)
            except serial.SerialException as e:
                # Transient USB-CDC hiccup on Windows ("ClearCommError failed /
                # device does not recognize the command") — close, drop DTR,
                # reopen, re-assert DTR, and keep streaming instead of crashing.
                print(f"\n[monitor] CDC error, reopening: {e}", flush=True)
                try:
                    s.close()
                except Exception:
                    pass
                time.sleep(0.5)
                try:
                    s = serial.Serial(port=a.port, baudrate=a.baud, timeout=0.05,
                                      rtscts=False, dsrdtr=False)
                    s.dtr = True
                    s.reset_input_buffer()
                    print(f"[monitor] reopened {a.port}", flush=True)
                except serial.SerialException as e2:
                    print(f"[monitor] reopen failed ({e2}); retrying...", flush=True)
                    time.sleep(1.0)
                continue
            if data:
                sys.stdout.write(data.decode("utf-8", errors="replace"))
                sys.stdout.flush()
            if deadline and time.time() > deadline:
                print("\n[monitor] timeout reached, exiting.")
                break
    except KeyboardInterrupt:
        print("\n[monitor] stopped.")
    finally:
        try:
            s.dtr = False
            s.close()
        except Exception:
            pass
    return 0

if __name__ == "__main__":
    sys.exit(main())
