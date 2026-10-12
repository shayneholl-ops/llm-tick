"""One-shot COM4 boot-log capture for ~70 s (gitignored output).

Usage: python tools/capture-boot-70.py
Writes tools/review-flash-boot-log.txt, then prints the key status lines.
"""
import os
import re
import serial
import time

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "review-flash-boot-log.txt")
PORT = "COM4"
BAUD = 115200
DURATION = 70

def open_with_retry(port, tries=10, delay=3):
    last = None
    for i in range(tries):
        try:
            s = serial.Serial(port, BAUD, timeout=1)
            time.sleep(0.2)
            # DTR high = console streaming on this board
            s.dtr = True
            return s
        except Exception as e:
            last = e
            time.sleep(delay)
    raise last

buf = []
with open_with_retry(PORT) as s:
    t0 = time.time()
    while time.time() - t0 < DURATION:
        data = s.readline()
        if data:
            line = data.decode("utf-8", "replace").rstrip("\r\n")
            buf.append(line)
            print(line, flush=True)

with open(OUT, "w", encoding="utf-8") as f:
    f.write("\n".join(buf) + "\n")
print(f"\n[saved {len(buf)} lines to {OUT}]")

keys = re.compile(r"WiFi UP|wifi:mode|softAP|tick\] running|Guru|config loaded|provisioned|rebooting|verify|sta ip|reset reason")
hits = [l for l in buf if keys.search(l)]
print("--- key lines ---")
for l in hits:
    print(l)
