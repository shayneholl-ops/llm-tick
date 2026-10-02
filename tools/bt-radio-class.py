#!/usr/bin/env python3
"""bt-radio-class.py — decide whether a Bluetooth device is Classic (BR/EDR) or BLE.

Positive control for the llm-tick F1 investigation: it enumerates ALL Bluetooth
devices Windows knows about (paired and unpaired, Classic AND LE) and prints each
with its address, then separately lists what the BLE advertisement watcher can
actually hear. A device that appears in the general list but NEVER in the BLE
advertisement list is a Classic-only (BR/EDR) device — which is why an
ESP32-S3 (BLE-only) central can never find it.

Usage:  python tools/bt-radio-class.py [--seconds N] [--find NAME]
"""
import argparse
import asyncio
import sys

from winsdk.windows.devices.bluetooth import (
    BluetoothDevice,
    BluetoothLEDevice,
)
from winsdk.windows.devices.bluetooth.advertisement import (
    BluetoothLEAdvertisementWatcher,
    BluetoothLEScanningMode,
)
from winsdk.windows.devices.enumeration import DeviceInformation


def fmt_mac(addr: int) -> str:
    return ":".join(f"{(addr >> (8 * i)) & 0xFF:02X}" for i in range(5, -1, -1))


async def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=20, help="BLE watch window")
    ap.add_argument("--find", default="Keychron", help="name substring to highlight")
    a = ap.parse_args()

    # ── 1. All Bluetooth devices Windows knows (Classic + LE) ────────────────
    print("=== Bluetooth devices Windows knows (paired + unpaired, Classic AND LE) ===")
    known = []
    try:
        sel = BluetoothDevice.get_device_selector()
        infos = await DeviceInformation.find_all_async(sel)
        for info in infos:
            name = info.name or ""
            addr = "?"
            # The device id embeds the address, e.g.
            #   Bluetooth#BluetoothDC:2C:26:EA:D3:E5
            try:
                parts = [p for p in info.id.split("#") if ":" in p]
                if parts:
                    addr = parts[-1].split("-")[0].upper()
            except Exception:
                pass
            known.append((name, addr, info.id))
    except Exception as e:
        print(f"  [enumeration failed: {e}]")

    keychron_addrs = set()
    for name, addr, _id in sorted(known):
        if "keychron" in name.lower() or (a.find and a.find.lower() in name.lower()):
            print(f"  >> {addr}  name='{name}'   <-- MATCH")
            keychron_addrs.add(addr)
        else:
            print(f"     {addr}  name='{name}'")
    print(f"  total: {len(known)} devices; matches: {len(keychron_addrs)}")

    # ── 2. What the BLE advertisement watcher can actually hear ──────────────
    print(f"\n=== BLE advertisements heard over {a.seconds}s (what an S3 can see) ===")
    heard = {}

    def on_received(_sender, e):
        adv = e.advertisement
        mac = fmt_mac(e.bluetooth_address)
        heard[mac] = adv.local_name or ""

    watcher = BluetoothLEAdvertisementWatcher()
    watcher.scanning_mode = BluetoothLEScanningMode.ACTIVE
    watcher.add_received(on_received)
    watcher.start()
    await asyncio.sleep(a.seconds)
    watcher.stop()

    print(f"  {len(heard)} BLE advertisers heard")
    for mac, name in sorted(heard.items()):
        mark = "  <-- also in the general list" if any(mac == k for _n, k, _i in known) else ""
        print(f"     {mac}  name='{name}'{mark}")

    # ── 3. Verdict ───────────────────────────────────────────────────────────
    print("\n=== VERDICT ===")
    if keychron_addrs:
        for addr in sorted(keychron_addrs):
            in_ble = addr in heard or any(
                fmt_mac(e.bluetooth_address) == addr for e in []
            )
            print(f"  '{a.find}' device {addr}: known to Windows, "
                  f"{'ALSO heard on BLE' if in_ble else 'NEVER heard on BLE'}")
        print("  => Windows holds this device over Classic (BR/EDR): it is discoverable/inquirable")
        print("     on the Classic radio only. A BLE-only central (ESP32-S3) cannot see or connect")
        print("     to it, no matter what the GATT client does.")
    else:
        print(f"  No device matching '{a.find}' found in the general Bluetooth list either.")
        print("  If you expected one, make sure it is powered on and in pairing mode.")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
