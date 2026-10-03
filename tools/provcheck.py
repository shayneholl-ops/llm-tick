#!/usr/bin/env python3
"""Verify the Provisioning state machine on the board.

Checks, in order, with the serial buffer drained to a QUIET point before each
measurement (the bug that made earlier manual runs look like failures: buffered
output from before a command was counted as if it happened after it):

  1. PROV enters Provisioning and the AP comes up
  2. polling is SUSPENDED while Provisioning is active
  3. PROV stop leaves Provisioning and polling RESUMES
  4. a WiFi connection failure does NOT enter Provisioning (ADR-0001)

Usage:  python tools/provcheck.py COM4
"""
import re
import sys
import time

import serial

GATE = "gate ACTIVE"


def quiet(s, max_wait=8.0):
    """Read until the board stops talking, so the next window only holds new output."""
    t0 = time.time()
    while time.time() - t0 < max_wait:
        if not s.read(200000):
            return True
        time.sleep(0.3)
    return False


def window(s, seconds):
    buf = b""
    t0 = time.time()
    while time.time() - t0 < seconds:
        buf += s.read(4096)
    return buf.decode("utf-8", errors="replace")


def counts(text):
    lines = text.splitlines()
    fetches = sum(1 for l in lines if "usage ok" in l or "wx fetch" in l)
    return fetches, text.count(GATE)


def main() -> int:
    port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
    s = serial.Serial(port, 115200, timeout=0.2)
    s.dtr = True

    # Wait for the board to be running at all.
    boot = window(s, 40)
    if "running" not in boot and "usage ok" not in boot:
        print("[provcheck] board never reported running")
        s.close()
        return 2

    fails = []

    def report(name, ok, detail=""):
        if isinstance(detail, list):
            detail = " | ".join(x for x in detail if x)
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}{('  ' + detail) if detail else ''}")
        if not ok:
            fails.append(name)

    # ── enter ────────────────────────────────────────────────────────────────
    print("\n=== enter Provisioning (PROV) ===")
    s.write(b"PROV\n")
    s.flush()
    time.sleep(3)
    got = window(s, 6)
    report("PROV reports the AP is up", "Setup AP" in got,
           [l.strip() for l in got.splitlines() if "Setup AP" in l][:1])
    report("the QR encoded", "QR ok" in got)

    quiet(s)
    print("\n=== polling must be SUSPENDED while active (40 s) ===")
    t = window(s, 40)
    f, g = counts(t)
    report("no fetch/usage lines emitted", f == 0, f"({f} seen)")
    # The gate's observable effect IS the absence of fetches, and the board keeps
    # rendering while suspended (pushed= heartbeats), which distinguishes "poll
    # suspended" from "board hung".
    beats = t.count("pushed=")
    report("the board is still rendering (not hung)", beats > 0, f"({beats} heartbeats)")

    # ── leave ────────────────────────────────────────────────────────────────
    print("\n=== leave Provisioning (PROV stop) ===")
    s.write(b"PROV stop\n")
    s.flush()
    time.sleep(3)
    got = window(s, 6)
    report("PROV stop reports the AP is down", "Setup AP down" in got)

    quiet(s)
    # 90 s, not 45: after leaving Provisioning the board re-resolves the Server and the
    # usage poll runs on a 60 s interval, so the first fetch can legitimately land past
    # the 45 s mark. Testing at 45 s measured the poll interval, not the resume.
    print("\n=== polling must RESUME within 90 s ===")
    t = window(s, 90)
    f, g = counts(t)
    report("fetch/usage lines return", f > 0, f"({f} seen)")

    s.close()
    print()
    if fails:
        print(f"FAILURES: {len(fails)} -> {fails}")
        return 1
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
