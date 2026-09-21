#!/usr/bin/env python3
"""llama_usage_poller.py — turn llama-server /metrics counters into a usage log.

The board's usage page is fed by server.py, which reads a JSONL token log:

    {"ts": <epoch seconds>, "model": "<name>", "input_tokens": N, "output_tokens": M}

llama-server (started with --metrics) exposes *cumulative* counters, so this
poller samples them every INTERVAL seconds, diffs them, and appends one row per
non-zero delta. Idle polls write nothing, so the log only grows when the model
actually serves tokens.

Notes
  * A llama-server restart resets the counters; a decrease is taken as a fresh
    baseline and produces no negative row.
  * Baseline is set by the first successful poll, so run it before/around a
    restart rather than after heavy use if you care about that gap.

Env knobs: LLAMA_BASE (default http://192.168.1.94:12345), OUT_PATH, INTERVAL.
"""
import json
import os
import re
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
BASE = os.environ.get("LLAMA_BASE", "http://192.168.1.94:12345").rstrip("/")
OUT = os.environ.get("OUT_PATH", os.path.join(HERE, "usage.jsonl"))
LOGF = os.environ.get("POLLER_LOG", os.path.join(HERE, "poller.log"))
INTERVAL = int(os.environ.get("INTERVAL", "30"))

IN_METRIC = "llamacpp:prompt_tokens_total"
OUT_METRIC = "llamacpp:tokens_predicted_total"


def log(msg):
    line = "%s %s" % (time.strftime("%Y-%m-%d %H:%M:%S"), msg)
    print(line, flush=True)
    try:
        with open(LOGF, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except OSError:
        pass


def get(path, timeout=10):
    with urllib.request.urlopen(BASE + path, timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")


def metric(text, name):
    m = re.search(r"^" + re.escape(name) + r"\s+([0-9.eE+-]+)\s*$", text, re.M)
    return float(m.group(1)) if m else None


def model_name():
    try:
        n = os.path.basename(json.loads(get("/props")).get("model_path") or "")
        if n.lower().endswith(".gguf"):
            n = n[:-5]
        return n or "llama-server"
    except Exception:
        return "llama-server"


def main():
    model = model_name()
    label = os.environ.get("MODEL_NAME") or model
    log("poller start base=%s out=%s interval=%ds model=%s label=%s"
        % (BASE, OUT, INTERVAL, model, label))
    prev = None
    while True:
        try:
            text = get("/metrics")
            a = metric(text, IN_METRIC)
            b = metric(text, OUT_METRIC)
            if a is None or b is None:
                raise RuntimeError("counters missing from /metrics (is --metrics set?)")
            if prev is not None:
                da = a - prev[0] if a >= prev[0] else a        # reset -> new baseline
                db = b - prev[1] if b >= prev[1] else b
                if da > 0 or db > 0:
                    row = {"ts": time.time(), "model": label,
                           "input_tokens": int(da), "output_tokens": int(db)}
                    with open(OUT, "a", encoding="utf-8") as f:
                        f.write(json.dumps(row) + "\n")
                    log("+%d in / +%d out -> %s" % (int(da), int(db), os.path.basename(OUT)))
            prev = (a, b)
        except Exception as e:
            log("poll error: %s" % e)
        time.sleep(INTERVAL)


if __name__ == "__main__":
    main()
