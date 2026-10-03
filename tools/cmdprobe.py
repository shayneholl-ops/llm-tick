#!/usr/bin/env python3
"""Find out whether the board is accepting serial commands, and how reliably.

The Board streams steady-state log lines continuously, and the host must drain to a
QUIET point before commanding or the reply gets mixed with (or preceded by) unrelated
output. This tool reports, per attempt, whether a command was answered — the
distinction that matters when a command appears to "not work".

Usage:  python tools/cmdprobe.py COM4 [attempts]
"""
import sys
import time

import serial

# Commands with a distinctive reply, so a reply is unambiguous.
PROBES = [
    ("PROV", "[prov]"),
    ("ST", "[diag]"),
    ("CFG", "[cfg] effective"),
]


def quiet(s, rounds=25, pause=0.25):
    """Drain until the board stops talking. Returns True if it went quiet."""
    for _ in range(rounds):
        if not s.read(200000):
            return True
        time.sleep(pause)
    return False


def main() -> int:
    port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
    attempts = int(sys.argv[2]) if len(sys.argv) > 2 else 2

    s = serial.Serial(port, 115200, timeout=0.3)
    s.dtr = True

    # Wait for evidence the board is running at all.
    t0 = time.time()
    buf = b""
    while time.time() - t0 < 40:
        buf += s.read(4096)
        if b"pushed=" in buf or b"usage ok" in buf:
            break
    else:
        print("[cmdprobe] the board never produced a steady-state line")
        s.close()
        return 2
    print("[cmdprobe] board is running\n")

    total = 0
    answered = 0
    for cmd, marker in PROBES:
        for i in range(attempts):
            total += 1
            quiet(s)
            s.write((cmd + "\n").encode())
            s.flush()
            out = ""
            t1 = time.time()
            hit = False
            while time.time() - t1 < 4.0:
                chunk = s.read(65536).decode("utf-8", errors="replace")
                if chunk:
                    out += chunk
                    if marker in out:
                        hit = True
                        break
            if hit:
                answered += 1
            print("  {:5} attempt {} -> {}".format(cmd, i + 1, "REPLY" if hit else "no reply"))
            if hit:
                for line in out.splitlines():
                    if "[prov]" in line or "[diag]" in line or "[cfg]" in line:
                        print("        " + line.strip())
                        break

    s.close()
    print("\n  answered {}/{}".format(answered, total))
    return 0 if answered == total else 1


if __name__ == "__main__":
    sys.exit(main())
