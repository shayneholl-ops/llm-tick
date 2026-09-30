# raw-kbd-watch.py — watch raw keyboard input on Windows, attributed per device.
#
# F1 verification helper: the ESP32-S3 bridge enumerates as a USB HID keyboard
# (VID_303A PID_1001, MI_00). This tool logs every key event that arrives from
# that specific device, so "the Keychron's keys came through the ESP32 bridge"
# can be proven from the host side instead of by feel.
#
# Usage:  python raw-kbd-watch.py [--vid 303A] [--pid 1001] [--list]
#   --list            print all raw keyboard devices and exit
#   --vid/--pid       restrict reporting to a specific vendor/product (hex)
# Without --vid/--pid every keyboard event is logged with its device name, so
# you can run it with --list first, then pin the match.
#
# Requires a message loop; run it and leave it running, ctrl-c to stop.

import ctypes
import ctypes.wintypes as wintypes
import sys
import time
from ctypes import wintypes as wt

USER32 = ctypes.windll.user32
USER32.DefWindowProcW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
USER32.DefWindowProcW.restype = ctypes.c_longlong
USER32.GetMessageW.argtypes = [ctypes.POINTER(wt.MSG), wt.HWND, wt.UINT, wt.UINT]
USER32.GetMessageW.restype = wt.BOOL
KERNEL32 = ctypes.windll.kernel32

# ── constants ────────────────────────────────────────────────────────────────
WM_INPUT = 0x00FF
WM_INPUT_DEVICE_CHANGE = 0x00FE
GIDC_ARRIVAL = 1
GIDC_REMOVAL = 2
RID_INPUT = 0x10000003
RIDEV_INPUTSINK = 0x00000100   # receive even when not foreground
RIDEV_DEVNOTIFY = 0x00002000
RIDI_DEVICENAME = 0x20000007
RI_KEY_MAKE = 0
RI_KEY_BREAK = 1
RI_KEY_E0 = 2
RI_KEY_E1 = 4

# ── structs ──────────────────────────────────────────────────────────────────
class RAWINPUTDEVICE(ctypes.Structure):
    _fields_ = [("usUsagePage", wt.USHORT),
                ("usUsage", wt.USHORT),
                ("dwFlags", wt.DWORD),
                ("hwndTarget", wt.HWND)]

class RAWKEYBOARD(ctypes.Structure):
    _fields_ = [("MakeCode", wt.USHORT),
                ("Flags", wt.USHORT),
                ("Reserved", wt.USHORT),
                ("VKey", wt.USHORT),
                ("Message", wt.UINT),
                ("ExtraInformation", wt.ULONG)]

class RAWINPUTHEADER(ctypes.Structure):
    _fields_ = [("dwType", wt.DWORD),
                ("dwSize", wt.DWORD),
                ("hDevice", wt.HANDLE),
                ("wParam", wt.WPARAM)]

class RAWINPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [("keyboard", RAWKEYBOARD)]
    _anonymous_ = ("u",)
    _fields_ = [("header", RAWINPUTHEADER),
                ("u", _U)]

class WNDCLASSEX(ctypes.Structure):
    _fields_ = [("cbSize", wt.UINT),
                ("style", wt.UINT),
                ("lpfnWndProc", ctypes.c_void_p),
                ("cbClsExtra", ctypes.c_int),
                ("cbWndExtra", ctypes.c_int),
                ("hInstance", wt.HINSTANCE),
                ("hIcon", wt.HICON),
                ("hCursor", wt.HANDLE),
                ("hbrBackground", wt.HBRUSH),
                ("lpszMenuName", wt.LPCWSTR),
                ("lpszClassName", wt.LPCWSTR),
                ("hIconSm", wt.HICON)]

WM_NCCREATE = 0x0081
WM_CREATE = 0x0001
WM_DESTROY = 0x0002

# ── device name helpers ──────────────────────────────────────────────────────
def dev_name_by_handle(h):
    n = wt.DWORD(0)
    USER32.GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, None, ctypes.byref(n))
    if n.value <= 0:
        return "?"
    buf = ctypes.create_unicode_buffer(n.value + 2)
    USER32.GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, buf, ctypes.byref(n))
    return buf.value

def parse_vid_pid(devname):
    # e.g. \\?\HID#VID_303A&PID_1001&MI_00#...
    vid = pid = None
    import re
    m = re.search(r"VID_([0-9A-Fa-f]{4})", devname)
    if m: vid = m.group(1).upper()
    m = re.search(r"PID_([0-9A-Fa-f]{4})", devname)
    if m: pid = m.group(1).upper()
    return vid, pid

# ── VKey → readable char (best effort) ───────────────────────────────────────
_keynames = {
    0x08: "Backspace", 0x09: "Tab", 0x0D: "Enter", 0x1B: "Esc",
    0x20: "Space", 0x25: "Left", 0x26: "Up", 0x27: "Right", 0x28: "Down",
    0x2D: "Insert", 0x2E: "Delete", 0x21: "PageUp", 0x22: "PageDown",
    0x24: "Home", 0x23: "End", 0x2C: "Snapshot", 0x90: "NumLock",
    0x14: "CapsLock", 0x70 + 0: "F1", 0x71: "F2", 0x72: "F3", 0x73: "F4",
    0x74: "F5", 0x75: "F6", 0x76: "F7", 0x77: "F8", 0x78: "F9",
    0x79: "F10", 0x7A: "F11", 0x7B: "F12",
    0xA0: "LShift", 0xA1: "RShift", 0xA2: "LCtrl", 0xA3: "RCtrl",
    0xA4: "LAlt", 0xA5: "RAlt", 0xA6: "LWin", 0xA7: "RWin",
}

def vk_to_char(vk, flags_e0):
    # printable ASCII via MapVirtualKey(VK, MAPVK_VK_TO_CHAR=2); modifiers
    # handled by keyb state of the focused app, so only map plain letters/digits
    if 0x30 <= vk <= 0x39:  # 0-9 (no shift handling; digits only)
        return chr(vk)
    if 0x41 <= vk <= 0x5A:  # A-Z (uppercase raw; shift state unknown here)
        return chr(vk)
    return None

# ── window proc (relay from C to python via a trampoline type) ───────────────
WNDPROC = ctypes.WINFUNCTYPE(
    ctypes.c_longlong if ctypes.sizeof(ctypes.c_void_p) == 8 else ctypes.c_long,
    wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)

_g_target_vid = None
_g_target_pid = None
_g_print_all = True
_g_first = True

def _wndproc(hwnd, msg, wparam, lparam):
    global _g_first
    if msg == WM_CREATE:
        return 0
    if msg == WM_INPUT:
        try:
            sz = wt.DWORD(0)
            USER32.GetRawInputData(ctypes.c_void_p(lparam), RID_INPUT, None,
                                   ctypes.byref(sz), ctypes.sizeof(RAWINPUT))
            if sz.value == 0:
                return 0
            buf = ctypes.create_string_buffer(sz.value)
            if USER32.GetRawInputData(ctypes.c_void_p(lparam), RID_INPUT, buf,
                                      ctypes.byref(sz),
                                      ctypes.sizeof(RAWINPUT)) != sz.value:
                return 0
            ri = ctypes.cast(buf, ctypes.POINTER(RAWINPUT)).contents
            if ri.header.dwType != 1:  # RIM_TYPEKEYBOARD
                return 0
            kbd = ri.keyboard
            name = dev_name_by_handle(ri.header.hDevice)
            vid, pid = parse_vid_pid(name)
            match = True
            if _g_target_vid is not None and vid != _g_target_vid:
                match = False
            if _g_target_pid is not None and pid != _g_target_pid:
                match = False
            if _g_print_all or match:
                if (kbd.Flags & RI_KEY_E0) or (kbd.Flags & RI_KEY_E1):
                    mod = "E0" if (kbd.Flags & RI_KEY_E0) else "E1"
                else:
                    mod = ""
                direction = "down" if not (kbd.Flags & (RI_KEY_BREAK | 0x80)) else "up"
                vk = kbd.VKey
                ch = ""
                if not direction == "up":
                    c = vk_to_char(vk, (kbd.Flags & RI_KEY_E0) != 0)
                    if c: ch = f"  '{c}'"
                label = _keynames.get(vk, f"VK_{vk:02X}")
                print(f"[raw] {time.strftime('%H:%M:%S')} {name}")
                print(f"      {direction:5s} {label:9s} {ch}  (mk={kbd.MakeCode:02X} flags={kbd.Flags:02X})",
                      flush=True)
        except Exception as e:  # keep the watcher alive on any hiccup
            print(f"[raw] err {e!r}", flush=True)
        return 0
    if msg == WM_INPUT_DEVICE_CHANGE:
        w = "arrival" if wparam == GIDC_ARRIVAL else ("removal" if wparam == GIDC_REMOVAL else str(wparam))
        print(f"[raw] {time.strftime('%H:%M:%S')} device {w}: {dev_name_by_handle(ctypes.c_void_p(lparam))}", flush=True)
        return 0
    if msg == WM_DESTROY:
        USER32.PostQuitMessage(0)
        return 0
    return USER32.DefWindowProcW(hwnd, msg, wparam, lparam)

# ── main ─────────────────────────────────────────────────────────────────────
def list_devices():
    n = wt.UINT(0)
    USER32.GetRawInputDeviceList(None, ctypes.byref(n), ctypes.sizeof(RAWINPUTDEVICE))
    class RID(ctypes.Structure):
        _fields_ = [("hDevice", wt.HANDLE), ("dwType", wt.DWORD)]
    if n.value == 0:
        print("(no raw input devices)")
        return
    devs = (RID * n.value)()
    USER32.GetRawInputDeviceList(devs, ctypes.byref(n), ctypes.sizeof(RID))
    for d in devs:
        if d.dwType == 1:  # keyboard
            print(f"{dev_name_by_handle(d.hDevice)}")

def main():
    global _g_target_vid, _g_target_pid, _g_print_all
    args = sys.argv[1:]
    if "--list" in args:
        list_devices()
        return
    if "--vid" in args:
        _g_target_vid = args[args.index("--vid") + 1].upper()
    if "--pid" in args:
        _g_target_pid = args[args.index("--pid") + 1].upper()
    _g_print_all = not (_g_target_vid or _g_target_pid)

    hinst = KERNEL32.GetModuleHandleW(None)
    wndproc = WNDPROC(_wndproc)
    cls = WNDCLASSEX()
    cls.cbSize = ctypes.sizeof(WNDCLASSEX)
    cls.lpfnWndProc = ctypes.cast(wndproc, ctypes.c_void_p)
    cls.hInstance = hinst
    cls.lpszClassName = "RawKbdWatch"
    if not USER32.RegisterClassExW(ctypes.byref(cls)):
        raise ctypes.WinError()
    hwnd = USER32.CreateWindowExW(0, "RawKbdWatch", "raw-kbd-watch",
                                  0, 0, 0, 0, 0, None, None, hinst, None)
    if not hwnd:
        raise ctypes.WinError()

    rid = (RAWINPUTDEVICE * 1)()
    rid[0].usUsagePage = 0x01
    rid[0].usUsage = 0x06            # keyboards
    rid[0].dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY
    rid[0].hwndTarget = hwnd
    if not USER32.RegisterRawInputDevices(rid, 1, ctypes.sizeof(RAWINPUTDEVICE)):
        raise ctypes.WinError()

    print("[raw] watching keyboard input", flush=True)
    if _g_target_vid or _g_target_pid:
        print(f"[raw] filtering: VID={_g_target_vid} PID={_g_target_pid}", flush=True)
    else:
        print("[raw] reporting ALL keyboard devices; use --vid/--pid to pin", flush=True)

    msg = wt.MSG()
    while USER32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
        USER32.TranslateMessage(ctypes.byref(msg))
        USER32.DispatchMessageW(ctypes.byref(msg))

if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print(f"[raw] fatal {e!r}", flush=True)
        sys.exit(1)