"""RTS-pulse reset of the board (back into APP mode, not download mode),
then capture ~75 s of console to catch the boot lines.

Usage: python tools/capture-reset-boot.py
Writes tools/review-flash-boot2-log.txt and prints the key status lines.
"""
import os
import re
import serial
import time

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "review-flash-boot2-log.txt")
PORT = "COM4"
BAUD = 115200
DURATION = 75

s = serial.Serial(PORT, BAUD, timeout=1)
s.dtr = False
s.rts = False
time.sleep(0.2)
# RTS = EN (reset) on this board's USB-Serial/JTAG wiring; DTR stays LOW so
# GPIO0 is not strapped to download mode -> chip boots straight into the app.
s.rts = True
time.sleep(0.12)
s.rts = False
s.dtr = True  # re-enable console streaming
print("[reset] RTS pulse sent; capturing", DURATION, "s")

buf = []
t0 = time.time()
while time.time() - t0 < DURATION:
    data = s.readline()
    if data:
        line = data.decode("utf-8", "replace").rstrip("\r\n")
        buf.append(line)
        print(line, flush=True)
s.close()

with open(OUT, "w", encoding="utf-8") as f:
    f.write("\n".join(buf) + "\n")
print(f"\n[saved {len(buf)} lines to {OUT}]")

keys = re.compile(r"WiFi UP|wifi:mode|softAP|tick\] running|Guru|config loaded|provisioned|rebooting|verify|sta ip|reset reason|boot:")
hits = [l for l in buf if keys.search(l)]
print("--- key lines ---")
for l in hits:
    print(l)
