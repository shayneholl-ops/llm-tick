#!/usr/bin/env python3
"""capture-new-macs.py — record every BLE MAC seen over a short window,
then print only those that were NOT in a prior baseline.

Usage:
  python tools/capture-new-macs.py <seconds> [baseline.jsonl]
"""
import json
import re
import sys
import asyncio
from collections import defaultdict

from winsdk.windows.devices.bluetooth.advertisement import (
    BluetoothLEAdvertisementWatcher, BluetoothLEScanningMode,
)

HID_UUID = "00001812-0000-1000-8000-00805f9b34fb"


def fmt_mac(addr: int) -> str:
    return ":".join(f"{(addr >> (8 * i)) & 0xFF:02X}" for i in range(5, -1, -1))


def main() -> int:
    seconds = int(sys.argv[1]) if len(sys.argv) > 1 else 30
    baseline = sys.argv[2] if len(sys.argv) > 2 else ""

    prior = set()
    if baseline:
        try:
            with open(baseline, "r", encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if line:
                        prior.add(json.loads(line)["mac"])
        except Exception as e:
            print(f"[baseline] failed to load: {e}")

    watcher = BluetoothLEAdvertisementWatcher()
    watcher.scanning_mode = BluetoothLEScanningMode.ACTIVE

    stats = defaultdict(lambda: {"name": "", "count": 0, "first_rssi": 0, "hid": False})

    def on_received(sender, e):
        adv = e.advertisement
        mac = fmt_mac(e.bluetooth_address)
        rssi = e.raw_signal_strength_in_d_bm
        name = adv.local_name or ""
        svcs = [str(u) for u in adv.service_uuids]
        hid = HID_UUID in svcs
        s = stats[mac]
        s["name"] = name or s["name"]
        s["count"] += 1
        if not s["first_rssi"]:
            s["first_rssi"] = rssi
        s["hid"] = s["hid"] or hid

    watcher.add_received(on_received)
    print(f"[capture] watching {seconds}s ...", flush=True)
    watcher.start()
    try:
        loop = asyncio.new_event_loop()
        loop.run_until_complete(asyncio.sleep(seconds))
    finally:
        watcher.stop()

    new = {m: s for m, s in stats.items() if m not in prior}
    print(f"\n[capture] total MACs seen: {len(stats)}, new vs baseline: {len(new)}")
    for mac in sorted(new, key=lambda m: -new[m]["count"]):
        s = new[mac]
        flag = " [HID]" if s["hid"] else ""
        print(f"[NEW] {mac} count={s['count']} name='{s['name']}'{flag}")

    print("\n[stats]")
    for mac, s in stats.items():
        print(json.dumps({"mac": mac, "name": s["name"], "count": s["count"],
                          "first_rssi": s["first_rssi"], "hid": s["hid"]}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
