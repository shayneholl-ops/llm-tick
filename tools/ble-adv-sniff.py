"""Ground-truth BLE advertisement sniffer on the HOST (Windows) Bluetooth adapter.

Reveals exactly how the Keychron K8 (and everything else) broadcasts, so we can see
why the ESP32-S3 scanner never detects it. Printed per packet: ADV type
(ADV_IND / ADV_DIRECT_IND / ADV_EXT_IND ...), MAC, RSSI, name, HID service 0x1812,
and the raw advertisement PDU bytes (to inspect legacy vs extended flags).

Requires: pip install winsdk  (already installed in llm-tick venv)
Usage:    python tools/ble-adv-sniff.py [--seconds 20] [--match Keychron] [--all]
"""
import argparse
import asyncio
import sys

from winsdk.windows.devices.bluetooth.advertisement import (
    BluetoothLEAdvertisementWatcher,
    BluetoothLEScanningMode,
)

HID_UUID = "00001812-0000-1000-8000-00805f9b34fb"


def fmt_mac(addr: int) -> str:
    return ":".join(f"{(addr >> (8 * i)) & 0xFF:02X}" for i in range(5, -1, -1))


def describe(adv_type):
    t = str(adv_type)
    mapping = {
        "ConnectableUndirected": "ADV_IND (legacy connectable undirected)",
        "ConnectableDirected": "ADV_DIRECT_IND (directed → only to a bonded host)",
        "ScannableUndirected": "ADV_SCAN_IND (legacy scannable undirected)",
        "NonConnectableUndirected": "ADV_NONCONN_IND (not connectable)",
        "Extended": "ADV_EXT_IND (extended advertising!)",
    }
    for k, v in mapping.items():
        if k in t:
            return v
    return t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=20)
    ap.add_argument("--match", default="Keychron")
    ap.add_argument("--all", action="store_true", help="print every packet, not just match")
    ap.add_argument("--log", default="", help="also append every packet's one-liner to this file")
    args = ap.parse_args()

    watcher = BluetoothLEAdvertisementWatcher()
    watcher.scanning_mode = BluetoothLEScanningMode.ACTIVE

    seen = {}

    def on_received(sender, e):
        adv = e.advertisement
        mac = fmt_mac(e.bluetooth_address)
        rssi = e.raw_signal_strength_in_d_bm
        adv_type = describe(e.advertisement_type)
        name = adv.local_name or ""
        svcs = [str(u) for u in adv.service_uuids]
        hid = "HID" if HID_UUID in svcs else "no-hid"

        mfr = []
        for d in adv.manufacturer_data:
            mfr.append(f"{d.company_id:04X}={bytes(d.data).hex()}")
        svc_data = [bytes(d.data).hex() for d in adv.data_sections if True]

        # raw sections hex dump
        sections_hex = []
        for s in adv.data_sections:
            b = bytes(s.data)
            sections_hex.append(f"[{b[0]:02X}]{b[1:].hex()}")

        mode = "extended" if "EXT" in adv_type else ("directed" if "directed" in adv_type else "legacy")

        line = (f"[sniff] {mac} rssi={rssi} {adv_type} name='{name}' "
                f"{hid} mfr={','.join(mfr) or '-'}")
        match_ok = (args.all or (args.match and args.match.lower() in name.lower()))
        if args.log:
            with open(args.log, "a", encoding="utf-8") as f:
                f.write(line + "\n")
        if match_ok:
            print(line)
            if sections_hex:
                print("        advsections: " + " ".join(sections_hex))
            seen[mac] = seen.get(mac, 0) + 1

    watcher.add_received(on_received)
    watcher.scanning_mode = BluetoothLEScanningMode.ACTIVE
    print(f"Sniffing BLE advertisements for {args.seconds}s on host adapter ...")
    watcher.start()
    try:
        loop = asyncio.new_event_loop()
        loop.run_until_complete(asyncio.sleep(args.seconds))
    finally:
        watcher.stop()

    print("\n=== Devices seen (name | mac | count) ===")
    for mac, n in sorted(seen.items(), key=lambda kv: -kv[1]):
        print(f"  {n:3d}  {mac}")
    if not seen:
        print("  (none matched)")


if __name__ == "__main__":
    main()
