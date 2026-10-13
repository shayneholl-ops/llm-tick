# try-download-dance.py — induce ESP32-S3 ROM download mode via the
# USB-Serial/JTAG DTR/RTS lines on COM4 (no user BOOT press).
#
# Board wiring (established last session): an RTS pulse on COM4 resets the
# chip; DTR low at the reset edge booted the app (IO0 high). So DTR likely
# gates IO0 the other way: DTR HIGH at the reset edge -> IO0 low -> download.
# The S3 ROM also has a built-in USB download check, so each candidate holds
# a DTR/RTS state THROUGH the reset edge, then polls for a new ROM COM port.
import sys, time
import serial
from serial.tools import list_ports

CONSOLE = 'COM4'
BAUD = 115200

def ports():
    return {p.device for p in list_ports.comports()}

before = ports()
print(f'[dance] ports before: {sorted(before)}', flush=True)

ser = serial.Serial(port=CONSOLE, baudrate=BAUD, timeout=0.1)

def set_state(dtr, rts, hold_ms=150):
    ser.dtr = dtr
    ser.rts = rts
    time.sleep(hold_ms / 1000.0)

def edge_and_poll(name, hold_dtr, hold_rts, edge_rts, tries=20):
    # 1) settle into the state the ROM will see AT the reset edge
    set_state(hold_dtr, hold_rts, 300)
    # 2) reset edge (RTS pulse) — or, variant 'usb-only': no edge, just state
    if edge_rts is not None:
        set_state(hold_dtr, edge_rts, 150)
    # 3) after the edge, settle into the post-reset state the ROM checks
    set_state(hold_dtr, hold_rts, 300)
    for i in range(tries):
        time.sleep(0.25)
        now = ports()
        new = now - before
        if new:
            print(f'[dance] {name}: DOWNLOAD PORT {sorted(new)}', flush=True)
            return sorted(new)
        if i % 4 == 3:
            print(f'[dance] {name}: still {sorted(now)}', flush=True)
    return None

# candidates: (name, dtr_at_edge, rts_at_edge, reset_edge)
CANDIDATES = [
    ('dtr-hi-rts-lo', 1, 0, 1),   # IO0 low at reset, classic download entry
    ('dtr-hi-rts-hi', 1, 1, 0),   # inverted ROM check
    ('dtr-lo-rts-hi', 0, 1, 0),   # the known APP-boot state, as control
    ('dtr-lo-rts-lo', 0, 0, 1),   # plain reset, control
]

found = None
for name, d, r, e in CANDIDATES:
    found = edge_and_poll(name, d, r, e)
    if found:
        break
    # calm state between candidates
    set_state(0, 0, 200)

ser.close()
if found:
    print(f'DOWNLOAD-PORT {found[0]}', flush=True)
    sys.exit(0)
print('NO-DOWNLOAD-PORT', flush=True)
sys.exit(1)
