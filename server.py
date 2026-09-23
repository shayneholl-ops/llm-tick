#!/usr/bin/env python3
"""
llm-tick usage server for the Waveshare ESP32-S3-LCD-1.47B display.

Serves a JSON blob (port 8266) that the board draws as usage bars. It is
SOURCE-AGNOSTIC — plug in whatever LLM usage feed you have:

  * Local llama.cpp (default): reads a token log the LLM writes per request.
    Point LOG_PATH at your jsonl (default ~/.llama.cpp/usage.jsonl).
  * Claude Code (optional): CCUSAGE_BIN=ccusage adds a per-model token-split page.
  * Any other API: edit the *_pct() functions to your shape.

Usage:
    python server.py                      # loopback only, llama.cpp log
    BIND_HOST=192.168.1.50 python server.py   # let the board reach it
    CCUSAGE_BIN=ccusage python server.py    # also feed ccusage token data
    LOG_PATH=/path/to/usage.jsonl python server.py
"""

import json
import os
import re
import subprocess
import time
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

PORT = int(os.environ.get("PORT", "8266"))
BIND_HOST = os.environ.get("BIND_HOST", "127.0.0.1")
ALLOW_PUBLIC_BIND = os.environ.get("ALLOW_PUBLIC_BIND") == "1"
LOG_PATH = os.path.expanduser(os.environ.get("LOG_PATH", "~/.llama.cpp/usage.jsonl"))
CCUSAGE_BIN = os.environ.get("CCUSAGE_BIN", "")
# Server-side re-read cadence. The board polls every 15 s while the usage page is
# displayed (60 s otherwise), so this stays under that to keep the GPU row live.
REFRESH = 10

# The display is a tiny status light: it should say "the LLM is busy right now",
# not "what happened this week". So the bars are driven by the *last* request
# (the active 5h window): session_pct = how full that window is, weekly_pct =
# this model's share of recent tokens. Adjust the window below to your taste.
ACTIVE_WINDOW_S = 5 * 3600
RECENT_WINDOW_S = 24 * 3600

# ── GPU telemetry (optional, for the GPU row on the usage page) ───────────────
# The model runs on a *different* box from this one, and that box exposes no HTTP
# telemetry (only llama-server's :12345, whose metrics carry no GPU counters), so
# we ask over SSH. rocm-smi --json is machine-readable and a sample costs ~0.35 s.
#   GPU_SSH      ssh target            (default admin-a8@192.168.1.94)
#   GPU_CARD     rocm-smi card key     (default card0 = the RX 7900 XTX; card1 is
#                the 680M iGPU and never serves the model)
#   GPU_DISABLE=1                     turns sampling off entirely
# Auth is key-based (BatchMode), so nothing here prompts or stores a password.
GPU_SSH = os.environ.get("GPU_SSH", "admin-a8@192.168.1.94")
GPU_CARD = os.environ.get("GPU_CARD", "card0")
GPU_ENABLED = os.environ.get("GPU_DISABLE") != "1"
GPU_REFRESH = 5          # s between good samples (board polls every 15 s)
GPU_ERROR_REFRESH = 30   # s to wait after a failure — no SSH retry storm
GPU_TIMEOUT = 6          # s hard cap on one sample

_gpu_cache = {"data": None, "ts": 0}


def gpu_stats():
    """Live load/temperature of the model host's GPU.

    Never raises: any failure yields gpu_ok=False and the row shows "--", so a
    dead link degrades the display instead of breaking the usage payload.
    """
    now = time.time()
    cached = _gpu_cache["data"]
    if cached is not None:
        ttl = GPU_REFRESH if cached.get("gpu_ok") else GPU_ERROR_REFRESH
        if (now - _gpu_cache["ts"]) < ttl:
            return cached
    if not GPU_ENABLED:
        return {"gpu_ok": False, "gpu_load_pct": -1, "gpu_temp_c": -1.0,
                "gpu_temp_junction_c": -1.0}
    try:
        raw = subprocess.run(
            ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=4",
             "-o", "StrictHostKeyChecking=accept-new", GPU_SSH,
             "rocm-smi --showuse --showtemp --json 2>/dev/null"],
            capture_output=True, text=True, timeout=GPU_TIMEOUT).stdout
        card = json.loads(raw).get(GPU_CARD, {})
        load = card.get("GPU use (%)")
        edge = card.get("Temperature (Sensor edge) (C)")
        junc = card.get("Temperature (Sensor junction) (C)")
        out = {
            "gpu_ok": load is not None,
            "gpu_load_pct": int(float(load)) if load is not None else -1,
            "gpu_temp_c": float(edge) if edge is not None else -1.0,
            "gpu_temp_junction_c": float(junc) if junc is not None else -1.0,
        }
    except Exception:
        out = {"gpu_ok": False, "gpu_load_pct": -1, "gpu_temp_c": -1.0,
               "gpu_temp_junction_c": -1.0}
    _gpu_cache["data"] = out
    _gpu_cache["ts"] = now
    return out

_cache = {"data": None, "ts": 0}


def private_ipv4_addresses():
    try:
        out = __import__("subprocess").check_output(
            ["ipconfig"] if os.name == "nt" else ["ifconfig"], text=True)
        addrs = re.findall(r"(\d+\.\d+\.\d+\.\d+)", out)
        out = " ".join(addrs)
    except Exception:
        return []
    found = []
    for a in re.findall(r"(\d+\.\d+\.\d+\.\d+)", out):
        ip = __import__("ipaddress").ip_address(a)
        if ip.is_private and not ip.is_loopback:
            found.append(a)
    return found


def check_bind_host(host):
    if host in ("0.0.0.0", "::"):
        addr = None
    else:
        try:
            addr = __import__("ipaddress").ip_address(host)
        except ValueError:
            return  # hostname — leave resolution to the socket
    if ALLOW_PUBLIC_BIND or (addr is not None and (addr.is_private or addr.is_loopback)):
        return
    where = "every interface" if addr is None else "a public address"
    raise SystemExit(f"Refusing to bind {host} ({where}); use a LAN IP or ALLOW_PUBLIC_BIND=1")


def pretty_model(name):
    if name.startswith("["):
        name = name.split("]", 1)[-1].strip()
    if name.startswith("claude-"):
        parts = name[len("claude-"):].split("-")
        if parts and len(parts[-1]) == 8 and parts[-1].isdigit():
            parts = parts[:-1]
        ver = ".".join(p for p in parts[1:] if p.isdigit())
        return f"{parts[0].capitalize()} {ver}".strip()
    return name[:14]


def _read_jsonl(path):
    if not path or not os.path.exists(path):
        return []
    rows = []
    try:
        with open(path, "r", encoding="utf-8", errors="ignore") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    rows.append(json.loads(line))
                except ValueError:
                    continue
    except OSError:
        return []
    return rows


def _tokens(row):
    n = 0
    for key in ("input_tokens", "output_tokens", "total_tokens", "n_prompt_tokens",
                "n_completion_tokens", "tokens"):
        n += int(row.get(key) or 0)
    return n


def llama_usage():
    """Per-model token counts from a local token log (last N days)."""
    now = time.time()
    rows = _read_jsonl(LOG_PATH)
    if not rows:
        return {"cc_ok": False, "cc_error": f"no log at {LOG_PATH}", "tok_models": []}
    total_recent = 0
    total_active = 0
    per_model = {}
    for r in rows:
        ts = r.get("ts") or r.get("time") or r.get("timestamp") or 0
        try:
            ts = float(ts)
        except (TypeError, ValueError):
            ts = now
        tok = _tokens(r)
        model = pretty_model(r.get("model") or r.get("name") or "?")
        per_model.setdefault(model, 0)
        per_model[model] += tok
        if ts >= now - RECENT_WINDOW_S:
            total_recent += tok
        if ts >= now - ACTIVE_WINDOW_S:
            total_active += tok
    week_tok = total_recent  # the board labels the weekly bar; reuse the recent total

    models = []
    for name in sorted(per_model, key=lambda m: -per_model[m]):
        tok = per_model[name]
        models.append({
            "name": name,
            "pct": round(100 * tok / week_tok) if week_tok else 0,
            "tok": _fmt_tokens(tok),
            "cost": 0.0,
        })
    return {
        "cc_ok": True,
        "cc_error": "",
        "tok_active": _fmt_tokens(total_active),
        "cost_active": 0.0,
        "burn_hr": round(total_active / 1_000_000 * 100 / (ACTIVE_WINDOW_S / 3600), 1) if total_active else 0.0,
        "tok_week": _fmt_tokens(week_tok),
        "cost_week": 0.0,
        "tok_models": models[:4],
        "_session_pct": round(100 * total_active / 2_000_000, 1) if total_active else 0.0,
    }


def ccusage_usage():
    """Optional Claude Code token source (adds a token-split page)."""
    if not CCUSAGE_BIN:
        return {"cc_ok": False, "cc_error": "ccusage disabled", "tok_models": []}
    import subprocess
    try:
        blocks = json.loads(subprocess.run([CCUSAGE_BIN, "blocks", "--json", "--active"],
                                           capture_output=True, text=True, timeout=90).stdout).get("blocks", [])
        weeks = json.loads(subprocess.run([CCUSAGE_BIN, "weekly", "--json"],
                                          capture_output=True, text=True, timeout=90).stdout).get("weekly", [])
    except Exception as e:
        return {"cc_ok": False, "cc_error": str(e)[:120], "tok_models": []}
    active = blocks[0] if blocks and blocks[0].get("isActive") else {}
    week = weeks[-1] if weeks else {}
    week_tok = week.get("totalTokens") or 0
    models = []
    for mb in sorted(week.get("modelBreakdowns", []), key=lambda m: -(m.get("cost") or 0)):
        tok = sum(mb.get(k) or 0 for k in
                  ("inputTokens", "outputTokens", "cacheCreationTokens", "cacheReadTokens"))
        models.append({"name": pretty_model(mb.get("modelName") or "?"),
                       "pct": round(100 * tok / week_tok) if week_tok else 0,
                       "tok": _fmt_tokens(tok), "cost": round(mb.get("cost") or 0, 2)})
    return {"cc_ok": True, "cc_error": "",
            "tok_active": _fmt_tokens(active.get("totalTokens") or 0),
            "cost_active": round(active.get("costUSD") or 0, 2),
            "burn_hr": round((active.get("burnRate") or {}).get("costPerHour") or 0, 1),
            "tok_week": _fmt_tokens(week_tok), "cost_week": round(week.get("totalCost") or 0, 2),
            "tok_models": models[:4], "_session_pct": None}


def _fmt_tokens(n):
    if n >= 1_000_000: return f"{n/1_000_000:.1f}M"
    if n >= 1_000: return f"{n/1_000:.0f}K"
    return str(n)


def get_usage():
    now = time.time()
    if _cache["data"] and (now - _cache["ts"]) < REFRESH:
        return _cache["data"]

    # Local feed first (the primary, always-present source).
    data = llama_usage()
    cc = ccusage_usage() if CCUSAGE_BIN else {"cc_ok": False, "cc_error": ""}
    # Prefer the ccusage token page when it answered, else the llama.cpp one.
    src = cc if cc.get("cc_ok") else data
    models = src.get("tok_models", [])

    out = {
        "demo": False,
        "session_pct": src.get("_session_pct") if src.get("_session_pct") is not None else 0,
        "session_reset_ts": now,
        "session_reset_min": max(0, int((now + ACTIVE_WINDOW_S - now) // 60)),
        "weekly_pct": models[0]["pct"] if models else 0,
        "weekly_reset_ts": now,
        "weekly_reset_day": time.strftime("%a %H:%M", time.localtime(now)),
        "models": models[:4],
        "status": "allowed",
        "ts": int(now),
        "demo": False,
        "cc_ok": bool(src.get("cc_ok")),
        "cc_error": src.get("cc_error", ""),
        "tok_active": src.get("tok_active", "0"),
        "cost_active": src.get("cost_active", 0.0),
        "burn_hr": src.get("burn_hr", 0.0),
        "tok_week": src.get("tok_week", "0"),
        "cost_week": src.get("cost_week", 0.0),
        "tok_models": models[:4],
        "spend_enabled": False, "spend_pct": -1, "spend_used": 0.0,
        "spend_limit": 0.0, "spend_cur": "",
    }
    out.update(gpu_stats())
    _cache["data"] = out
    _cache["ts"] = now
    return out


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path in ("/", "/usage"):
            body = json.dumps(get_usage()).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_error(404)

    def log_message(self, fmt, *args):
        print(f"[{time.strftime('%H:%M:%S')}] {fmt % args}")


if __name__ == "__main__":
    check_bind_host(BIND_HOST)
    print(f"llm-tick usage server on port {PORT} (log: {LOG_PATH})")
    if is_loopback := (BIND_HOST == "127.0.0.1" or BIND_HOST == "localhost"):
        cands = ", ".join(private_ipv4_addresses()) or "(none found)"
        print(f"  Loopback only — the board cannot reach this.")
        print(f"  For the display rerun as:  BIND_HOST=<ip> python server.py")
        print(f"  LAN addresses on this machine: {cands}")
    server = ThreadingHTTPServer((BIND_HOST, PORT), Handler)
    print("Serving. Ctrl+C to stop.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down.")
        server.server_close()
