#!/usr/bin/env python3
"""Exercise the board's serial config surface and report reliability.

Sends each command and polls for the expected reply instead of sleeping a fixed
time, so a slow reply is not mistaken for a lost one. This is the harness that
distinguishes "the command was dropped" from "the reply arrived late" — the
distinction that mattered when diagnosing the CDC RX overflow.

Usage:
  python tools/serialcfg.py COM4                 # reliability sweep
  python tools/serialcfg.py COM4 "SETLOC Rome"   # one command, print the reply
"""
import sys
import time

import serial

EXPECT = {
    "CFG": "[cfg] effective",
    "ST": "[diag] scene=",
    "SETLOC": "[cfg] weather location saved",
    "SETWIFI": "[cfg] wifi",
    "SETSERVER": "[cfg] server",
    "FACTORY": "FACTORY",
    "PROV stop": "Setup AP down",
    "PROV": "[prov]",
}


def expected_for(cmd: str) -> str:
    # Longest-prefix first: "PROV stop" must not match the "PROV" entry.
    for key in sorted(EXPECT, key=len, reverse=True):
        if cmd.startswith(key):
            return EXPECT[key]
    return ""


def send(s, cmd: str, timeout: float = 4.0):
    """Send one command; return (reply_text, matched).

    Drains to a quiet point FIRST (see drain_quiet), then keeps reading for the whole
    timeout window even after a match, so trailing output from the same exchange is
    captured. Returning at the first match truncated replies and made commands look
    lost when they had in fact answered.
    """
    drain_quiet(s)
    s.write((cmd + "\n").encode())
    s.flush()
    want = expected_for(cmd)
    got = ""
    matched = False
    t0 = time.time()
    while time.time() - t0 < timeout:
        chunk = s.read(65536).decode("utf-8", errors="replace")
        if chunk:
            got += chunk
            if want and want in got:
                matched = True
                # Give the rest of this exchange a moment to arrive, then stop.
                time.sleep(0.25)
                got += s.read(65536).decode("utf-8", errors="replace")
                break
        if matched:
            break
    return got, matched


def wait_ready(s, timeout: float = 40.0) -> bool:
    """Wait until the Board is demonstrably running, rather than sleeping blind.

    The CDC only starts streaming once the host asserts DTR, and a command sent before
    the stream is up is silently lost. Waiting for a steady-state log line is the
    reliable ready signal; a fixed sleep is not.

    The timeout is generous because a cold boot takes ~20 s: WiFi association, NTP,
    then the first usage fetch. Testing at a fixed 2 s produced an 11/15 "failure" rate
    that was entirely a harness artefact — the Board had executed every command.
    """
    t0 = time.time()
    buf = b""
    while time.time() - t0 < timeout:
        buf += s.read(4096)
        if b"pushed=" in buf or b"usage ok" in buf:
            return True
    return False


def drain_quiet(s, rounds: int = 30, pause: float = 0.25) -> bool:
    """Read until the Board stops talking.

    REQUIRED before sending a command. The Board streams steady-state log lines
    continuously, and if the host does not clear that backlog first, the command's
    reply arrives mixed with unrelated output — which reads as "no reply" and makes a
    working command look broken. This was the single biggest source of false failures
    while building the config surface.
    """
    for _ in range(rounds):
        if not s.read(200000):
            return True
        time.sleep(pause)
    return False


def main() -> int:
    port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
    s = serial.Serial(port, 115200, timeout=0.3)
    s.dtr = True
    if not wait_ready(s):
        print("[serialcfg] board never produced a steady-state line; is it running?")
        s.close()
        return 2
    return run(s)

def run(s) -> int:
    if len(sys.argv) > 2:
        rc = 0
        for cmd in sys.argv[2:]:
            got, ok = send(s, cmd)
            print(f">>> {cmd}")
            for line in got.splitlines():
                if "[cfg]" in line or "[diag]" in line or "[prov]" in line:
                    print("   ", line)
            print(f"    [{'OK' if ok else 'NO REPLY'}]")
            if not ok:
                rc = 1
        s.close()
        return rc

    # Reliability sweep: each command repeated, reporting the ratio.
    cmds = []
    for c in ("CFG", "ST"):
        cmds += [c] * 5
    cmds += ["SETLOC Vancouver", "SETLOC Rome", "SETLOC Vancouver"]
    cmds += ["CFG", "ST"]

    ok = 0
    for c in cmds:
        _, hit = send(s, c)
        ok += hit
        print(f"  {c:22} -> {'OK' if hit else 'NO REPLY'}")
    s.close()
    print(f"\n  replies: {ok}/{len(cmds)}")
    return 0 if ok == len(cmds) else 1


if __name__ == "__main__":
    sys.exit(main())
