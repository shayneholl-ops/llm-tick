#!/usr/bin/env python3
"""boot-capture.py — attach to COM5 the INSTANT it enumerates and dump the boot log.

The llm-tick app (ESP32-S3 TinyUSB CDC) prints its boot banner once in the first
~1-2 s after power-on ([llm-tick] booting ... [ble] NimBLE init done ... [tick] running),
then only periodic logs. The generic monitor-com.py reopens slowly enough that it
misses the boot banner. This script busy-reopens COM5 with no backoff so it latches
within milliseconds of enumeration, then prints everything verbatim until the user
(host) asks it to stop or a timeout passes.

Usage:
  python tools/boot-capture.py COM5 [BAUD] [--timeout N]
  # keep it running, THEN unplug+replug the board's USB-C; the banner is captured.
"""
import argparse
import sys
import time

import serial


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("port", nargs="?", default="COM5")
    p.add_argument("baud", nargs="?", type=int, default=115200)
    p.add_argument("--timeout", type=float, default=45,
                   help="exit after N seconds total (default 45)")
    a = p.parse_args()

    # Open COM's usually get a different number each replug; also probe for the
    # board by scanning the common CDC port numbers for the first that opens.
    ports_to_try = [a.port] + [f"COM{i}" for i in range(3, 13) if f"COM{i}" != a.port]
    print(f"[boot] waiting for a serial device (try order: {ports_to_try}) ...",
          flush=True)

    s = None
    deadline = time.time() + a.timeout
    # Phase 1: wait until any candidate port opens.
    while time.time() < deadline:
        for port in ports_to_try:
            try:
                s = serial.Serial(port=port, baudrate=a.baud, timeout=0.3,
                                  rtscts=False, dsrdtr=False)
                s.dtr = True
                try:
                    s.reset_input_buffer()
                except Exception:
                    pass
                print(f"[boot] ATTACHED {port} @ {a.baud}, DTR set — streaming from now",
                      flush=True)
                break
            except serial.SerialException:
                s = None
        if s is not None:
            break
        time.sleep(0.05)  # aggressive ~50ms retry loop
    if s is None:
        print(f"[boot] no port opened within {a.timeout}s")
        return 1

    # Phase 2: stream everything (reopen-on-error) until timeout.
    while time.time() < deadline:
        if s is None:
            # Nothing latched (relatch failed or CD gone) — try to reconnect.
            reattached = False
            for port in ports_to_try:
                try:
                    s = serial.Serial(port=port, baudrate=a.baud, timeout=0.3,
                                      rtscts=False, dsrdtr=False)
                    s.dtr = True
                    print(f"[boot] re-attached {port}", flush=True)
                    reattached = True
                    break
                except serial.SerialException:
                    s = None
            if not reattached:
                time.sleep(0.05)
                continue
        try:
            data = s.read(256)
        except serial.SerialException:
            # CD gone (unplug) or transient — mark and relatch at loop top.
            try:
                s.close()
            except Exception:
                pass
            s = None
            continue
        if data:
            sys.stdout.write(data.decode("utf-8", errors="replace"))
            sys.stdout.flush()
        if a.timeout and time.time() > deadline:
            break
    try:
        s.close()
    except Exception:
        pass
    print("\n[boot] done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
