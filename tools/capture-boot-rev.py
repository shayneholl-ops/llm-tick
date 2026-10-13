# capture-boot-rev.py — one-shot: pulse an RTS reset on COM4 (chip reboots
# into the app), then capture ~45 s of boot serial to a log file.
# Usage: python tools/capture-boot-rev.py [seconds] [logfile]
import sys, time
import serial

port = 'COM4'
secs = int(sys.argv[1]) if len(sys.argv) > 1 else 45
log = sys.argv[2] if len(sys.argv) > 2 else 'tools/boot-rev-serial-log.txt'

with open(log, 'a', encoding='utf-8') as f:
    f.write(f'\n===== capture start {time.strftime("%Y-%m-%d %H:%M:%S")} (RTS reset) =====\n')
    ser = serial.Serial(port=port, baudrate=115200, timeout=0.2)
    # DTR LOW at the reset edge -> app boot (high would land in download mode)
    ser.dtr = False
    ser.rts = False
    time.sleep(0.3)
    # RTS pulse = chip reset (established kill-switch behaviour)
    ser.rts = True
    time.sleep(0.15)
    ser.rts = False
    f.write('===== RTS reset pulse sent =====\n')
    end = time.time() + secs
    while time.time() < end:
        line = ser.readline().decode('utf-8', 'replace').rstrip()
        if line:
            f.write(line + '\n')
    ser.close()
    f.write('===== capture end =====\n')
print(f'wrote {log}')
