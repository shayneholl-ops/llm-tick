#!/usr/bin/env python3
"""#5 stress driver: N Provisioning cycles, watching serial for the WiFi-RX
heap-corruption signature (HANDOFF item 5) and recording free heap per cycle.

One cycle = PROV (enter) -> HTTP GET the page -> POST the form with the real
credentials -> verify -> resolve -> reboot -> board returns to usage polling.
That is the radio load profile the ticket is about: AP beaconing, an HTTP
server accepting connections, AP+STA on one radio, a client loading a page.

This process owns COM4 for the whole run; do not run a monitor or esptool
alongside it. Passphrase is read from gitignored src/secrets.h and never
printed. Usage:
  python tools/stress-prov.py [cycles]        (default 10)
Logs:  tools/stress-prov-serial.txt (raw console)
       tools/stress-prov-heap.csv   (one row per observed freeHeap sample)
"""
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, "..", "src"))
SERIAL_LOG = os.path.join(HERE, "stress-prov-serial.txt")
HEAP_CSV = os.path.join(HERE, "stress-prov-heap.csv")
PORT = "COM4"
BAUD = 115200
BASE = "http://192.168.4.1/"

# Crash signatures for the documented WiFi-RX heap corruption (HANDOFF item 5)
# plus the generic assert/watchdog/panic ends. A hit aborts the run: the ticket
# says capture it and hand it to diagnosis, not patch blind.
CRASH = [
    "wifi:ebuf_free: invalid type",
    "Guru Meditation",
    "panic'ed",
    "abort() was called",
    "task_wdt:",
    "assert failed",
    "EXCVADDR",
    "Backtrace:",
    "StoreProhibited",
    "LoadProhibited",
]

HEAP_RE = re.compile(r"freeHeap=(\d+)")
CYCLE_RE = re.compile(r"\[cycle (\d+)\]")

# The Setup AP is a compile-time constant in the firmware (src/prov.h), not a
# secret. A cycle is only a real radio-load cycle if a client actually JOINS:
# the documented crash fires on association + the burst that follows it.
AP_SSID = "llm-tick-prov"
AP_PASS = "setup1234"
PROFILE = "llmtick-stress"


def wlan_join():
    """Associate the host to the Setup AP. Returns True once it has an IP."""
    xml = (
        '<?xml version="1.0"?><WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">'
        f"<name>{PROFILE}</name><SSIDConfig><SSID><name>{AP_SSID}</name></SSID></SSIDConfig>"
        "<connectionType>ESS</connectionType><connectionMode>manual</connectionMode>"
        "<MSM><security><authEncryption><authentication>WPA2PSK</authentication>"
        "<encryption>AES</encryption><useOneX>false</useOneX></authEncryption>"
        f"<sharedKey><keyType>passPhrase</keyType><protected>false</protected><keyMaterial>{AP_PASS}</keyMaterial>"
        "</sharedKey></security></MSM></WLANProfile>"
    )
    p = os.path.join(HERE, ".llmtick-stress.xml")
    with open(p, "w", encoding="utf-8") as f:
        f.write(xml)
    subprocess.run(["netsh", "wlan", "add", "profile", f"filename={p}", "interface=WiFi"],
                   capture_output=True, text=True)
    subprocess.run(["netsh", "wlan", "connect", f"name={PROFILE}", "ssid=" + AP_SSID, "interface=WiFi"],
                   capture_output=True, text=True)
    for _ in range(30):
        out = subprocess.run(["netsh", "wlan", "show", "interfaces"], capture_output=True, text=True).stdout
        if AP_SSID in out and "connected" in out.lower():
            return True
        time.sleep(1)
    return False


def wlan_leave():
    subprocess.run(["netsh", "wlan", "disconnect", "interface=WiFi"], capture_output=True, text=True)
    # Wait for the adapter to actually reach `disconnected` before the next cycle
    # sends PROV. A cycle that starts while the radio is still tearing its old
    # association down is a host-side race, not a Board finding: it showed up as
    # cycle 4 "no-ap" (the AP came up fine; our read window missed it).
    for _ in range(20):
        out = subprocess.run(["netsh", "wlan", "show", "interfaces"], capture_output=True, text=True).stdout
        if "disconnected" in out.lower():
            return True
        time.sleep(1)
    return False


def load_pass():
    with open(os.path.join(SRC, "secrets.h"), "r", encoding="utf-8") as f:
        m = re.search(r'WIFI_PASS\s+"([^"]+)"', f.read())
    if not m:
        raise SystemExit("WIFI_PASS not found in src/secrets.h")
    return m.group(1)


def get(timeout=5):
    r = urllib.request.urlopen(BASE, timeout=timeout)
    return r.status, r.read().decode("utf-8", "replace")


def fields(html):
    out = {}
    for m in re.finditer(r"<input\b[^>]*>", html):
        t = m.group(0)
        name = re.search(r'name="([^"]+)"', t)
        if not name:
            continue
        val = re.search(r'value="([^"]*)"', t)
        out[name.group(1)] = val.group(1) if val else ""
    return out


def notice(html):
    m = re.search(r'<p class="notice[^"]*"[^>]*>(.*?)</p>', html, re.S)
    return " ".join(m.group(1).split()) if m else "(no notice)"


class Console:
    """Owns COM4; pumps lines, records heap, trips on a crash signature."""

    def __init__(self):
        self.f = open(SERIAL_LOG, "w", encoding="utf-8")
        print(f"[stress] raw console -> {SERIAL_LOG}", flush=True)
        self.s = serial.Serial(PORT, BAUD, timeout=0.3)
        self.s.dtr = True
        self.crash = None
        self.heap_rows = []
        self.cycles_seen = set()

    def send(self, cmd):
        # Drain first, and never close before reading: writing into a port with
        # unread input (or right before close) silently loses the command on this
        # USB-Serial/JTAG console. That cost two lost `PROV stop` sends.
        self.s.reset_input_buffer()
        self.s.write((cmd + "\n").encode())
        self.s.flush()
        print(f"[stress] -> {cmd!r}", flush=True)

    def pump(self, seconds):
        """Read for `seconds`, printing and classifying. Returns lines seen."""
        end = time.time() + seconds
        lines = []
        while time.time() < end:
            data = self.s.readline()
            if not data:
                continue
            line = data.decode("utf-8", "replace").rstrip("\r\n")
            self.f.write(line + "\n")
            self.f.flush()
            lines.append(line)
            print(line, flush=True)
            for sig in CRASH:
                if sig in line:
                    self.crash = line
                    return lines
            m = HEAP_RE.search(line)
            if m:
                self.heap_rows.append((int(time.time()), int(m.group(1)), line))
            m = CYCLE_RE.search(line)
            if m:
                self.cycles_seen.add(int(m.group(1)))
        return lines

    def wait_for(self, needle, seconds):
        """Pump until `needle` appears; return everything seen either way, so the
        caller can still inspect a line that arrived while it was waiting."""
        end = time.time() + seconds
        seen = []
        while time.time() < end:
            seen.extend(self.pump(1.0))
            if needle in "\n".join(seen):
                break
        return seen

    def close(self):
        self.f.close()
        self.s.close()
        with open(HEAP_CSV, "w", encoding="utf-8") as f:
            f.write("epoch_s,free_heap,line\n")
            for row in self.heap_rows:
                f.write(f"{row[0]},{row[1]},\"{row[2]}\"\n")
        print(f"[stress] heap samples -> {HEAP_CSV} ({len(self.heap_rows)} rows)", flush=True)


def one_cycle(c, n, passwd):
    print(f"\n===== cycle {n} =====", flush=True)
    c.send("PROV")
    # "up (WPA2)" only prints on a fresh enter; on the second and later cycles the
    # AP is already up and the board answers "already in Provisioning" instead.
    # 25 s, not 6: a softAP re-init plus a host that has just left the previous
    # association can genuinely take longer than one DHCP round.
    seen = c.wait_for("up (WPA2)", 25)
    if "up (WPA2)" not in "\n".join(seen) and "already in Provisioning" not in "\n".join(seen):
        return "no-ap"
    if not wlan_join():
        print(f"[cycle {n}] host could not associate to {AP_SSID}", flush=True)
        return "no-assoc"
    print(f"[cycle {n}] host associated to {AP_SSID}", flush=True)
    # The board is now carrying its real load: AP beaconing, an HTTP server, and
    # a client that just associated. Sample the console for a few seconds before
    # touching the page, because the documented crash fires within ~1 s of
    # association — and grab the stack watermark, which only prints while a
    # client is attached.
    c.pump(5)
    if c.crash:
        return "crash"
    time.sleep(2.0)
    try:
        st, html = get()
    except Exception as e:
        print(f"[cycle {n}] GET failed: {type(e).__name__} {e}", flush=True)
        return "get-failed"
    print(f"[cycle {n}] GET {st}  notice: {notice(html)}", flush=True)
    f = fields(html)
    f["pass"] = passwd
    body = urllib.parse.urlencode(f).encode("utf-8")
    req = urllib.request.Request(BASE, data=body, method="POST")
    try:
        r = urllib.request.urlopen(req, timeout=15)
        print(f"[cycle {n}] POST {r.status}  notice: {notice(r.read().decode('utf-8', 'replace'))}", flush=True)
    except (urllib.error.URLError, OSError) as e:
        # Expected once the board reboots out from under us.
        print(f"[cycle {n}] POST transport: {type(e).__name__} {str(e)[:80]}", flush=True)
    # Verify -> resolve -> grace -> reboot. The console goes quiet at the reset, so
    # wait for the BOOT banner of the next life, not for any pre-reboot string.
    if not c.wait_for("[llm-tick] booting", 120):
        print(f"[cycle {n}] board did not reboot within 120 s", flush=True)
        return "no-return"
    if not c.wait_for("[tick] WiFi UP", 60):
        print(f"[cycle {n}] rebooted but no station link within 60 s", flush=True)
        return "no-wifi"
    print(f"[cycle {n}] back on the station network", flush=True)
    # The board is now a station again; the host must follow it off the Setup AP
    # so the next cycle starts from a clean, disconnected radio.
    wlan_leave()
    # Let it render one usage scene so "normal polling" is proven, not assumed.
    time.sleep(6)
    # Confirm the board is actually polling, not merely booted: the ticket asks
    # that it "return to normal usage polling after each cycle without manual
    # intervention", and the scene line is the only in-band proof of that.
    #
    # 45 s, because the boot→poll path is not one step: after "[tick] WiFi UP" the
    # board resolves mDNS, then runs its first fetch — and the usage server on
    # 192.168.1.94 is currently down, so that fetch burns its connect timeout
    # (`[tick] fetch HTTP -1`) before the scene renders. A 12 s window produced a
    # false "no-poll"; the board rendered fine ~20 s later, unchanged.
    saw_poll = False
    for line in c.pump(45):
        if "[tick] scene=" in line:
            saw_poll = True
            break
    if c.crash:
        return "crash"
    if not saw_poll:
        print(f"[cycle {n}] no scene render after the reboot", flush=True)
        return "no-poll"
    return "ok"


def main():
    cycles = int(sys.argv[1]) if len(sys.argv) > 1 else 10
    passwd = load_pass()
    c = Console()
    results = []
    try:
        print("[stress] waiting for board on this port (any console traffic)…", flush=True)
        c.pump(3)
        for n in range(1, cycles + 1):
            r = one_cycle(c, n, passwd)
            results.append(r)
            if c.crash:
                print(f"\n[stress] CRASH SIGNATURE at cycle {n}: {c.crash}", flush=True)
                break
            if r != "ok":
                print(f"[stress] cycle {n} ended: {r} — stopping (manual look needed)", flush=True)
                break
    finally:
        c.close()
    print("\n[stress] cycles:", results, flush=True)
    if c.crash:
        sys.exit(2)
    if len(results) < cycles or any(r != "ok" for r in results):
        sys.exit(1)


if __name__ == "__main__":
    main()
