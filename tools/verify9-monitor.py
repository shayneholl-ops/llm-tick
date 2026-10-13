"""Long-running COM4 monitor for the #9 hardware verify (gitignored log).

Streams the console to tools/verify9-serial-log.txt (and stdout) until killed.
While running, if tools/.serial-cmd appears, its contents are written to the
board as one command (the file is then deleted). Usage:
  python tools/verify9-monitor.py          # run (until killed)
  echo FACTORY > tools/.serial-cmd        # inject a command
"""
import os
import serial
import time

HERE = os.path.dirname(os.path.abspath(__file__))
LOG = os.path.join(HERE, "verify9-serial-log.txt")
CMDFILE = os.path.join(HERE, ".serial-cmd")
PORT = "COM4"
BAUD = 115200

open(LOG, "w", encoding="utf-8").close()
s = serial.Serial(PORT, BAUD, timeout=0.5)
s.dtr = True
print("[monitor] COM4 open, DTR on; log ->", LOG, flush=True)

with open(LOG, "a", encoding="utf-8") as f:
    while True:
        if os.path.exists(CMDFILE):
            try:
                with open(CMDFILE, "r", encoding="utf-8") as c:
                    cmd = c.read().strip()
                os.remove(CMDFILE)
                if cmd:
                    s.write((cmd + "\n").encode())
                    print(f"[monitor] injected: {cmd!r}", flush=True)
            except Exception as e:
                print(f"[monitor] cmd-file error: {e!r}", flush=True)
        data = s.readline()
        if data:
            line = data.decode("utf-8", "replace").rstrip("\r\n")
            f.write(line + "\n")
            f.flush()
            print(line, flush=True)
