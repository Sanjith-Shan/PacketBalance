#!/usr/bin/env python3
"""Render results/*.jsonl (and results/exp5_hash_quality.json) as markdown tables.

  python3 lab/render_tables.py            print every table to stdout
  python3 lab/render_tables.py --write    also replace the text between
                                          <!-- results:expN --> and <!-- /results:expN -->
                                          in README.md (exp1..exp6). Missing markers
                                          are reported and skipped.

Every cell over repeats is "mean (min-max)" with n = number of repeats shown
per row. Rows whose forwarding plane failed carry their note instead.
"""
import collections
import json
import os
import re
import statistics
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(ROOT, "results")


def load(name):
    path = os.path.join(RES, name)
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                try:
                    rows.append(json.loads(line))
                except ValueError:
                    pass
    return rows


def fmt_rate(v):
    if v is None:
        return "n/a"
    if v >= 1e6:
        return "%.2f M" % (v / 1e6)
    if v >= 1e3:
        return "%.0f k" % (v / 1e3)
    return "%.0f" % v


def fmt_num(v, digits=0):
    if v is None:
        return "n/a"
    return ("%%.%df" % digits) % v


def spread(values, fmt):
    vals = [v for v in values if isinstance(v, (int, float))]
    if not vals:
        return "n/a"
    m = statistics.mean(vals)
    if len(vals) == 1:
        return fmt(m)
    return "%s (%s-%s)" % (fmt(m), fmt(min(vals)), fmt(max(vals)))


def config_label(r):
    lb = r.get("lb", "?")
    if lb == "packetbalance":
        parts = ["PacketBalance", r.get("xdp_mode") or "?"]
        if r.get("hash") and r.get("hash") != "maglev":
            parts.append(r["hash"])
        if r.get("conntrack") is False:
            parts.append("no conntrack")
        return " ".join(parts)
    if lb.startswith("ipvs"):
        parts = lb.split("-")
        mode = "IPIP tunnel" if parts[-1] == "tun" else "DR"
        return "IPVS %s (%s)" % (parts[1], mode)
    if lb == "none":
        return "no LB (direct to real1)"
    return lb


def group(rows, key):
    g = collections.OrderedDict()
    for r in rows:
        g.setdefault(key(r), []).append(r)
    return g


def provenance(rows):
    if not rows:
        return ""
    hosts = sorted({"%s, %s CPUs, kernel %s, %s" % (r.get("host"), r.get("cpus"), r.get("kernel"), r.get("arch"))
                    for r in rows})
    dates = sorted(r.get("date", "")[:10] for r in rows)
    gens = sorted({r.get("generator") for r in rows if r.get("generator")})
    s = "Host: %s. Dates: %s to %s." % ("; ".join(hosts), dates[0], dates[-1])
    if gens:
        s += " Generator: %s." % ", ".join(gens)
    return s


def exp1_table(rows, title="Experiment 1: packet rate at saturation"):
    if not rows:
        return None
    ps = rows[0].get("pkt_size")
    out = ["### %s" % title, "",
           "%s-byte UDP frames (%s-byte skb), %s flows, %s s windows. " %
           (ps, rows[0].get("pkt_size_skb"), rows[0].get("flows"), round(rows[0].get("duration_s") or 0)) +
           provenance(rows), "",
           "| Configuration | n | Offered pps | Forwarded pps | Received at reals pps | VM CPU busy % | pps per core (est.) |",
           "|---|---:|---:|---:|---:|---:|---:|"]
    for label, rs in group(rows, config_label).items():
        ok = [r for r in rs if r.get("received_pps") is not None]
        if not ok:
            out.append("| %s | 0 | %s | | | | |" % (label, rs[0].get("notes", "failed")))
            continue
        out.append("| %s | %d | %s | %s | %s | %s | %s |" % (
            label, len(ok),
            spread([r.get("offered_pps") for r in ok], fmt_rate),
            spread([r.get("forwarded_pps") for r in ok], fmt_rate),
            spread([r.get("received_pps") for r in ok], fmt_rate),
            spread([r.get("lb_cpu_util") for r in ok], lambda v: fmt_num(v, 1)),
            spread([r.get("pps_per_core") for r in ok], fmt_rate)))
    out += ["", "pps per core = forwarded pps / (busy fraction x CPUs), over the whole VM, which also runs "
            "the generator and the reals; see results/README.md. For the no-LB row it is received pps."]
    return "\n".join(out)


def sweep_table(rows):
    if not rows:
        return None
    out = ["#### Offered load sweep (10 s per point, one repeat)", "",
           "| Configuration | Target pps | Offered pps | Forwarded pps | Received pps | VM CPU busy % |",
           "|---|---:|---:|---:|---:|---:|"]
    for r in sorted(rows, key=lambda r: (config_label(r), r.get("target_rate_pps") or 1e18)):
        t = r.get("target_rate_pps")
        out.append("| %s | %s | %s | %s | %s | %s |" % (
            config_label(r), "unthrottled" if not t else fmt_rate(t), fmt_rate(r.get("offered_pps")),
            fmt_rate(r.get("forwarded_pps")), fmt_rate(r.get("received_pps")), fmt_num(r.get("lb_cpu_util"), 1)))
    return "\n".join(out)


def exp2_table(rows):
    if not rows:
        return None
    out = ["### Experiment 2: HTTP latency and throughput through the LB", "",
           "`wrk -t4 -c256 -d30s --latency` from the client namespace against nginx. " + provenance(rows), "",
           "| Configuration | n | Requests/s | p50 | p99 | Socket errors |", "|---|---:|---:|---:|---:|---:|"]
    us = lambda v: "%.0f us" % v if v < 1000 else "%.2f ms" % (v / 1000)
    for label, rs in group(rows, config_label).items():
        ok = [r for r in rs if r.get("req_per_s")]
        if not ok:
            out.append("| %s | 0 | %s | | | |" % (label, rs[0].get("notes", "failed")))
            continue
        errs = [sum((r.get("socket_errors") or {}).values()) for r in ok]
        out.append("| %s | %d | %s | %s | %s | %s |" % (
            label, len(ok), spread([r["req_per_s"] for r in ok], lambda v: "%.0f" % v),
            spread([r.get("p50_us") for r in ok], us), spread([r.get("p99_us") for r in ok], us),
            spread(errs, lambda v: "%.0f" % v)))
    return "\n".join(out)


def cause(r, k):
    return (r.get("broken_by_cause") or {}).get(k, 0)


def exp3_table(rows):
    if not rows:
        return None
    out = ["### Experiment 3: connection survival through backend churn", "",
           "conncheck holds 10,000 TCP connections (heartbeat 100 ms, timeout 2 s). real3 is removed at t=10 s "
           "and added back at t=20 s. Correct: only the connections on real3 break. " + provenance(rows), "",
           "| Configuration | n | Established | On real3 at start | Broken | Broken on real3 | Broken elsewhere | rst / eof / timeout / wrong backend |",
           "|---|---:|---:|---:|---:|---:|---:|---|"]
    for label, rs in group(rows, config_label).items():
        ok = [r for r in rs if "broken" in r]
        if not ok:
            out.append("| %s | 0 | %s | | | | | |" % (label, rs[0].get("notes", "failed")))
            continue
        on3 = [(r.get("backends_at_start") or {}).get("3", 0) for r in ok]
        b3 = [(r.get("broken_by_backend") or {}).get("3", 0) for r in ok]
        other = [r["broken"] - x for r, x in zip(ok, b3)]
        causes = " / ".join(spread([cause(r, k) for r in ok], lambda v: "%.0f" % v)
                            for k in ("rst", "eof", "timeout", "wrong_backend"))
        f = lambda v: "%.0f" % v
        out.append("| %s | %d | %s | %s | %s | %s | %s | %s |" % (
            label, len(ok), spread([r.get("established") for r in ok], f), spread(on3, f),
            spread([r["broken"] for r in ok], f), spread(b3, f), spread(other, f), causes))
    return "\n".join(out)


def exp4_label(r):
    s = config_label(r)
    if r.get("ipvs_sloppy_tcp") is not None:
        s += ", sloppy_tcp=%d" % r["ipvs_sloppy_tcp"]
    return s


def exp4_table(rows):
    if not rows:
        return None
    out = ["### Experiment 4: connection survival through LB failover", "",
           "conncheck holds 10,000 TCP connections through lb1; at t=10 s the client's route flips to lb2, "
           "which has the same configuration and an empty connection table. Drift: lb2 also has real5. "
           + provenance(rows), "",
           "| Configuration | Drift | n | Established | Broken | Broken % | Minimum possible % |",
           "|---|---|---:|---:|---:|---:|---:|"]
    for (label, drift), rs in group(rows, lambda r: (exp4_label(r), bool(r.get("drift")))).items():
        ok = [r for r in rs if "broken" in r]
        if not ok:
            out.append("| %s | %s | 0 | %s | | | |" % (label, "real5 on lb2" if drift else "none",
                                                       rs[0].get("notes", "failed")))
            continue
        pct = [100.0 * r["broken"] / r["established"] for r in ok if r.get("established")]
        mn = ok[0].get("min_fraction_moved")
        out.append("| %s | %s | %d | %s | %s | %s | %s |" % (
            label, "real5 on lb2" if drift else "none", len(ok),
            spread([r.get("established") for r in ok], lambda v: "%.0f" % v),
            spread([r["broken"] for r in ok], lambda v: "%.0f" % v),
            spread(pct, lambda v: "%.1f" % v), "%.0f" % (100 * mn) if mn else "0"))
    return "\n".join(out)


def exp5_table():
    path = os.path.join(RES, "exp5_hash_quality.json")
    if not os.path.exists(path):
        return None
    d = json.load(open(path))
    out = ["### Experiment 5: hash quality", "",
           "%s synthetic flows through the control plane's rings (size %s), date %s." %
           (d.get("tuples"), d.get("ring_size"), d.get("date")), "",
           "| Mode | Real | Weight | Expected share | Observed share |", "|---|---|---:|---:|---:|"]
    for m in d.get("modes", []):
        for s in m.get("shares", []):
            out.append("| %s | %s | %s | %.2f%% | %.2f%% |" % (m["mode"], s["real"], s["weight"],
                                                              100 * s["expected"], 100 * s["observed"]))
    out += ["", "| Mode | Event | Minimum possible | Flows moved | Slots moved |", "|---|---|---:|---:|---:|"]
    for m in d.get("modes", []):
        for e in m.get("disruption", []):
            out.append("| %s | %s | %.2f%% | %.2f%% | %.2f%% |" % (m["mode"], e["event"], 100 * e["target"],
                                                                 100 * e["flows_moved"], 100 * e["slots_moved"]))
    return "\n".join(out)


def exp6_table(rows):
    if not rows:
        return None
    out = ["### Experiment 6: what the connection table costs", "",
           "Experiment 1's load through PacketBalance with the connection table off and at three sizes. "
           + provenance(rows), "",
           "| Connection table | n | Forwarded pps | Received pps | VM CPU busy % | pps per core (est.) | Notes |",
           "|---|---:|---:|---:|---:|---:|---|"]
    key = lambda r: "off" if r.get("conntrack") is False else "%s flows (--conntrack-size)" % format(r.get("conntrack_size") or 0, ",")
    for label, rs in group(rows, key).items():
        ok = [r for r in rs if r.get("forwarded_pps") is not None]
        notes = "" if ok else (rs[0].get("notes") or "")[:120]
        out.append("| %s | %d | %s | %s | %s | %s | %s |" % (
            label, len(ok), spread([r.get("forwarded_pps") for r in ok], fmt_rate),
            spread([r.get("received_pps") for r in ok], fmt_rate),
            spread([r.get("lb_cpu_util") for r in ok], lambda v: fmt_num(v, 1)),
            spread([r.get("pps_per_core") for r in ok], fmt_rate), notes))
    return "\n".join(out)


def build():
    t1 = exp1_table(load("exp1_pps.jsonl"))
    sw = sweep_table(load("exp1_sweep.jsonl"))
    if t1 and sw:
        t1 = t1 + "\n\n" + sw
    return collections.OrderedDict([
        ("exp1", t1 or sw),
        ("exp2", exp2_table(load("exp2_http.jsonl"))),
        ("exp3", exp3_table(load("exp3_churn.jsonl"))),
        ("exp4", exp4_table(load("exp4_failover.jsonl"))),
        ("exp5", exp5_table()),
        ("exp6", exp6_table(load("exp6_conntrack.jsonl"))),
    ])


def main():
    tables = build()
    for k, t in tables.items():
        print(t if t else "### %s: no results yet" % k)
        print()
    if "--write" not in sys.argv:
        return
    readme = os.path.join(ROOT, "README.md")
    if not os.path.exists(readme):
        print("README.md not found; printed only", file=sys.stderr)
        return
    text = open(readme).read()
    changed = False
    for k, t in tables.items():
        if not t:
            continue
        pat = re.compile(r"(<!-- results:%s -->)(.*?)(<!-- /results:%s -->)" % (k, k), re.S)
        if not pat.search(text):
            print("README.md has no <!-- results:%s --> markers; skipped" % k, file=sys.stderr)
            continue
        text = pat.sub(lambda m: m.group(1) + "\n" + t + "\n" + m.group(3), text)
        changed = True
    if changed:
        open(readme, "w").write(text)
        print("README.md updated", file=sys.stderr)


if __name__ == "__main__":
    main()
