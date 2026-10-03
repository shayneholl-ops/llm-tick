#!/usr/bin/env python3
"""llm-tick serial console helper: send a command, return the reply.

One command per invocation is often too slow when exercising the config surface, so
this takes several and prints the whole exchange. It also asserts the reply contains
an expected substring when given, which is what makes the acceptance checks scriptable.

Usage:
  python tools/llmserial.py COM4 "CFG"
  python tools/llmserial.py COM4 --expect "wifi saved" "SETWIFI TestNet secret123"
  python tools/llmserial.py COM4 --wait 6 -- "FACTORY"
"""
import argparse
import sys
import time

import serial


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--wait", type=float, default=2.5, help="seconds to read after each command")
    ap.add_argument("--pre", type=float, default=0.0, help="seconds to read before sending")
    ap.add_argument("--expect", default="", help="substring the reply must contain")
    ap.add_argument("cmds", nargs="+")
    a = ap.parse_args()

    try:
        s = serial.Serial(a.port, a.baud, timeout=0.2)
    except serial.SerialException as e:
        print(f"[serial] cannot open {a.port}: {e}")
        return 2
    s.dtr = True

    if a.pre:
        time.sleep(a.pre)
        s.read(65536)          # drain boot chatter

    full = ""
    for cmd in a.cmds:
        # Deliberately NO reset_input_buffer() here. The CDC keeps streaming
        # steady-state log lines, and a reply to the previous command may still be
        # arriving; draining the buffer discards exactly the output we want to read.
        s.write((cmd + "\n").encode())
        s.flush()
        time.sleep(a.wait)
        got = s.read(65536).decode("utf-8", errors="replace")
        print(f"  >>> {cmd}")
        for line in got.splitlines():
            if line.strip():
                print(f"      {line}")
        full += got
    s.close()

    if a.expect:
        ok = a.expect in full
        print(f"  [{'OK' if ok else 'MISSING'}] expected to see: {a.expect!r}")
        return 0 if ok else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
