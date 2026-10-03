#!/usr/bin/env python3
"""Regression test for server.py's per-model share maths.

The bug (found 2026-10-02): `weekly_pct` divided LIFETIME per-model tokens by the
TRAILING-24H total. With a ledger spanning days, the numerator could exceed the
denominator by orders of magnitude — the board displayed `weekly=14953604%`.

The contract the board relies on (src/ui.cpp):
  * `weekly_pct`  = the top model's share, as a percentage  -> drives the bar
  * `tok_week`    = the headline token count for that bar
  * `models[].pct`= each model's share, same window as the total
  * `models[].tok`= that model's tokens, same window as the total
Every one of those must come from ONE window, or the bar and its number disagree.

Run:  python tools/test-server-windows.py
"""
import importlib.util
import json
import os
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.join(os.path.dirname(HERE), "server.py")


def load_server(log_path):
    os.environ["LOG_PATH"] = log_path
    os.environ["GPU_DISABLE"] = "1"
    spec = importlib.util.spec_from_file_location("srv_under_test", SERVER)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def write_ledger(rows):
    fd, path = tempfile.mkstemp(suffix=".jsonl", prefix="ledger-")
    os.close(fd)
    with open(path, "w", encoding="utf-8") as f:
        for ts, model, tin, tout in rows:
            f.write(json.dumps({"ts": ts, "model": model,
                                "input_tokens": tin, "output_tokens": tout}) + "\n")
    return path


def check(name, got, want, tol=0):
    ok = abs(got - want) <= tol if isinstance(want, (int, float)) else got == want
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}: got {got!r}, want {want!r}")
    return ok


def main():
    now = time.time()
    day = 24 * 3600
    ok = True

    # ── Case 1: the original bug. Rows INSIDE the window must be the only ones
    # that count; a huge lifespan of older rows must not inflate the share. ──
    # Window (7d): only the two rows at -6d and -60s are inside.
    print("\nCase 1: large out-of-window history must not inflate the share")
    p = write_ledger([
        (now - 90 * day, "Qwen3.8-27B", 9_000_000, 1_000_000),  # far outside window
        (now - 2 * day,  "Qwen3.8-27B", 60, 40),                # inside window
        (now - 3 * day,  "Qwen3-27B",   100, 0),                # inside window
    ])
    s = load_server(p)
    d = s.get_usage()
    # Window total = 100 (modelA) + 100 (modelB) = 200 -> top share = 50%.
    ok &= check("weekly_pct is a real share, not millions", d["weekly_pct"], 50, tol=1)
    ok &= check("tok_week is a token string", isinstance(d["tok_week"], str), True)
    for m in d["models"]:
        if m["pct"] > 100:
            print(f"  [FAIL] a model share exceeded 100%: {m['name']}={m['pct']}%")
            ok = False
        if m["tok"].endswith("M") and m["pct"] == 0:
            print(f"  [FAIL] {m['name']} shows a lifetime token count for a 0% share")
            ok = False
    os.unlink(p)

    # ── Case 2: two models active in the SAME window -> shares must sum to ~100. ──
    print("\nCase 2: two models active inside the window")
    p = write_ledger([
        (now - 3600, "modelA", 75, 0),
        (now - 3600, "modelB", 25, 0),
    ])
    s = load_server(p)
    d = s.get_usage()
    total_share = sum(m["pct"] for m in d["models"])
    ok &= check("shares sum to ~100", total_share, 100, tol=2)
    ok &= check("top model share", d["weekly_pct"], 75, tol=1)
    os.unlink(p)

    # ── Case 3: everything historical -> window is empty, no crash, no fake %. ──
    print("\nCase 3: all rows older than the window (empty window)")
    p = write_ledger([
        (now - 60 * day, "modelA", 5000, 5000),
    ])
    s = load_server(p)
    d = s.get_usage()
    ok &= check("weekly_pct is 0 with an empty window", d["weekly_pct"], 0)
    ok &= check("no share above 100", all(m["pct"] <= 100 for m in d["models"]), True)
    os.unlink(p)

    # ── Case 4: a model in the window AND a model only in history. ──
    # The historical model's *window* tokens are 0, so it must not appear as a
    # claimant on the window, and its `tok` must not be its lifetime total.
    print("\nCase 4: historical-only model must not claim a share of the window")
    p = write_ledger([
        (now - 60 * day, "oldmodel", 8_000_000, 0),
        (now - 600,      "newmodel", 400, 100),
    ])
    s = load_server(p)
    d = s.get_usage()
    by_name = {m["name"]: m for m in d["models"]}
    ok &= check("newmodel is the only claimant, so 100%", d["weekly_pct"], 100)
    if "Oldmodel" in by_name:
        ok &= check("oldmodel window share is 0", by_name["Oldmodel"]["pct"], 0)
    else:
        print("  [PASS] oldmodel not listed (its window tokens are 0)")
    ok &= check("tok_week counts only window rows", d["tok_week"], "500")
    os.unlink(p)

    print("\n" + ("ALL PASS" if ok else "FAILURES PRESENT"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
