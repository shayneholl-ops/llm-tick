#!/usr/bin/env python3
"""One check for server.py: feed it a fake token log, confirm the JSON shape
and the busy/idle percentages behave. Run:  python server_test.py
"""
import json
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import server


def make_log(path, rows):
    with open(path, "w") as f:
        for r in rows:
            f.write(json.dumps(r) + "\n")


def test_shape_and_math():
    now = time.time()
    with tempfile.NamedTemporaryFile("w", suffix=".jsonl", delete=False) as f:
        path = f.name
    try:
        # 800k tokens in the last hour (active 5h window), 1.2M total last 24h.
        make_log(path, [
            {"ts": now - 1800, "model": "qwen3-27b", "input_tokens": 500000, "output_tokens": 300000},
            {"ts": now - 7200, "model": "qwen3-27b", "input_tokens": 600000, "output_tokens": 200000},
            {"ts": now - 7200, "model": "deepseek-v4", "input_tokens": 200000, "output_tokens": 100000},
        ])
        server.LOG_PATH = path
        server._cache["data"] = None
        d = server.get_usage()

        assert set(["session_pct", "weekly_pct", "status", "models", "tok_models"]) <= set(d)
        assert d["status"] == "allowed"
        assert d["cc_ok"] is True
        assert 0 <= d["session_pct"] <= 100
        assert d["models"][0]["name"] in ("qwen3-27b", "deepseek-v4")
        # most-recent model should be first and hold the majority share
        assert d["models"][0]["name"] == "qwen3-27b"
        assert d["models"][0]["pct"] > d["models"][1]["pct"]
        print("OK  shape + per-model ordering")

        # Idle: clear the log -> session_pct should drop to 0.
        make_log(path, [])
        server._cache["data"] = None
        d2 = server.get_usage()
        assert d2["session_pct"] == 0, d2["session_pct"]
        print("OK  idle -> session_pct 0")
    finally:
        os.unlink(path)


if __name__ == "__main__":
    test_shape_and_math()
    print("server_test passed")
