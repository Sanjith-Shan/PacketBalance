#!/usr/bin/env python3
"""Small JSON helpers for the lab shell scripts.

  labutil.py row [--merge FILE.json]... key=value key:=<json> ...
      Print one JSON row: host, cpus, kernel, arch and date first, then every
      merged file's top-level keys, then the given keys. key=value stores a
      string, key:=value stores parsed JSON (numbers, true/false/null, lists).
      LAB_HOST comes from the environment (lab/host.env).

  labutil.py wrk FILE
      Parse `wrk --latency` output and print JSON: req_per_s, transfer,
      latency avg/stdev/max and p50/p75/p90/p99 in microseconds, socket errors,
      non-2xx responses, total requests.
"""
import datetime
import json
import os
import platform
import re
import sys


def to_us(text):
    m = re.fullmatch(r"([0-9.]+)(us|ms|s|m)", text.strip())
    if not m:
        return None
    v, unit = float(m.group(1)), m.group(2)
    return round(v * {"us": 1, "ms": 1e3, "s": 1e6, "m": 60e6}[unit], 1)


def parse_wrk(path):
    out = {}
    text = open(path).read()
    m = re.search(r"Latency\s+(\S+)\s+(\S+)\s+(\S+)", text)
    if m:
        out["latency_avg_us"] = to_us(m.group(1))
        out["latency_stdev_us"] = to_us(m.group(2))
        out["latency_max_us"] = to_us(m.group(3))
    for pct in ("50", "75", "90", "99"):
        m = re.search(r"^\s+%s%%\s+(\S+)" % pct, text, re.M)
        out["p%s_us" % pct] = to_us(m.group(1)) if m else None
    m = re.search(r"Requests/sec:\s+([0-9.]+)", text)
    out["req_per_s"] = float(m.group(1)) if m else None
    m = re.search(r"(\d+) requests in ([0-9.]+\w+)", text)
    out["requests"] = int(m.group(1)) if m else None
    m = re.search(r"Socket errors: connect (\d+), read (\d+), write (\d+), timeout (\d+)", text)
    out["socket_errors"] = (
        dict(zip(("connect", "read", "write", "timeout"), map(int, m.groups()))) if m else {}
    )
    m = re.search(r"Non-2xx or 3xx responses: (\d+)", text)
    out["non_2xx"] = int(m.group(1)) if m else 0
    return out


def row(args):
    r = {
        "host": os.environ.get("LAB_HOST", "unknown host"),
        "cpus": os.cpu_count(),
        "kernel": platform.release(),
        "arch": platform.machine(),
        "date": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
    }
    it = iter(args)
    for a in it:
        if a == "--merge":
            path = next(it)
            try:
                with open(path) as f:
                    r.update(json.load(f))
            except (OSError, ValueError) as exc:
                r.setdefault("errors", []).append("merge %s: %s" % (path, exc))
        elif ":=" in a:
            k, v = a.split(":=", 1)
            r[k] = json.loads(v)
        elif "=" in a:
            k, v = a.split("=", 1)
            r[k] = v
        else:
            sys.exit("labutil row: bad argument %r" % a)
    print(json.dumps(r))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd, rest = sys.argv[1], sys.argv[2:]
    if cmd == "row":
        row(rest)
    elif cmd == "wrk":
        print(json.dumps(parse_wrk(rest[0])))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
