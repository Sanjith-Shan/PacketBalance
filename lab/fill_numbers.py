#!/usr/bin/env python3
"""Turn results/ into the numbers the docs, the ledger and the resume use.

  python3 lab/fill_numbers.py --check    every headline number with provenance, and flags
  python3 lab/fill_numbers.py --fill     fill {{PLACEHOLDERS}} from docs/templates/ into
                                         docs/CAPACITY.md and the interview defense file,
                                         then run render_tables.py --write for the README
  python3 lab/fill_numbers.py --ledger   NUMBERS_LEDGER.md rows, to stdout
  python3 lab/fill_numbers.py --bullets  the two resume bullets plus alternates, to stdout
  python3 lab/fill_numbers.py --fill --dry-run DIR   fill into DIR only, touch nothing else

Options: --basis forwarded|received picks the pps-per-core basis for Exp 1 and Exp 6
(default forwarded, which is what results/README.md defines; received counts only what
arrived at the reals). --defense PATH overrides the interview file (env PB_DEFENSE_DOC).

Rows are loaded and grouped with render_tables.py's own functions. The math here is
mean, min and max over repeats, the same as its tables. Nothing is invented: a quantity
without rows is MISSING, and a placeholder that cannot be filled stays in the file.
--fill always starts from docs/templates/, so rerunning after new rows replaces old
numbers. Standard library only.
"""
import argparse
import collections
import json
import math
import os
import re
import shutil
import statistics
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import render_tables as rt  # noqa: E402

ROOT, RES = rt.ROOT, rt.RES
TEMPLATES = os.path.join(ROOT, "docs", "templates")
DEFENSE_DEFAULT = os.path.expanduser("~/Documents/MasterIntern/interview/meta/packetbalance-defense.md")
SPREAD_LIMIT = 15.0  # percent, (max - min) / mean
LINE_CHARS = 105     # rough characters per rendered resume line; the real check is the resume build


# ---------------------------------------------------------------------------
# quantities
# ---------------------------------------------------------------------------
class Q:
    """One headline number: values over repeats, the rows behind them, and its kind."""

    def __init__(self, key, desc, values, rows, kind, src, total=None, note=""):
        self.key, self.desc, self.kind, self.src, self.note = key, desc, kind, src, note
        self.values = [v for v in values if isinstance(v, (int, float)) and not isinstance(v, bool)]
        self.rows, self.total, self.flags = rows, total, []

    missing = property(lambda s: not s.values)
    n = property(lambda s: len(s.values))
    mean = property(lambda s: statistics.mean(s.values) if s.values else None)
    lo = property(lambda s: min(s.values) if s.values else None)
    hi = property(lambda s: max(s.values) if s.values else None)

    @property
    def spread_pct(self):
        if not self.values:
            return None
        if self.mean == 0:
            return 0.0 if self.hi == self.lo else math.inf
        return 100.0 * (self.hi - self.lo) / abs(self.mean)

    def consistent(self):
        if self.missing:
            return False
        if self.kind == "count":  # counts: within 15% or within 1% of the connections held
            slack = max(0.15 * abs(self.mean), 0.01 * (self.total or 0))
            return self.hi - self.lo <= slack
        return self.spread_pct <= SPREAD_LIMIT

    def verified(self):
        # Exp 5 is one deterministic document (fixed seed), so repeats do not apply to it
        need = 1 if self.src.endswith(".json") else 3
        return self.n >= need and self.consistent() and not self.flags


def commit_of(r):
    m = re.search(r"commit ([0-9a-f]{7,40})([^;]*)", r.get("notes") or "")
    if not m:
        return "commit not recorded"
    return (m.group(1) + " " + m.group(2).split(",")[0].strip()).strip()


def prov(rows):
    """Provenance fields over a set of rows, as a dict of strings."""
    if not rows:
        return {}
    uniq = lambda k: sorted({str(r.get(k)) for r in rows if r.get(k) is not None})
    dates = sorted((r.get("date") or "")[:10] for r in rows if r.get("date"))
    return {
        "host": "; ".join(uniq("host")) or "?",
        "cpus": ", ".join(uniq("cpus")) or "?",
        "kernel": ", ".join(uniq("kernel")) or "?",
        "arch": ", ".join(uniq("arch")) or "?",
        "xdp_mode": ", ".join(uniq("xdp_mode")) or "n/a",
        "pkt_size": ", ".join(uniq("pkt_size")) or "n/a",
        "flows": ", ".join(format(int(x), ",") for x in {r["flows"] for r in rows if r.get("flows")}) or "n/a",
        "conntrack_size": ", ".join(format(int(x), ",") for x in {r["conntrack_size"] for r in rows
                                                                 if r.get("conntrack_size")}) or "n/a",
        "n": str(len(rows)),
        "date": (dates[0] if dates and dates[0] == dates[-1] else "%s to %s" % (dates[0], dates[-1])) if dates else "?",
        "commit": "; ".join(sorted({commit_of(r) for r in rows})),
    }


def prov_line(q):
    p = prov(q.rows)
    if not p:
        return "no rows"
    if q.src.endswith(".json"):
        return "control plane test tool, machine independent, %s, date %s" % (q.rows[0]["notes"], p["date"])
    return ("host %(host)s, %(cpus)s CPUs, kernel %(kernel)s %(arch)s, xdp_mode %(xdp_mode)s, "
            "pkt_size %(pkt_size)s, flows %(flows)s, conntrack_size %(conntrack_size)s, n=%(n)s, "
            "date %(date)s, %(commit)s" % p)


# ---------------------------------------------------------------------------
# loading and grouping (render_tables.load / group)
# ---------------------------------------------------------------------------
def exp1_key(r):
    if r.get("config"):
        return r["config"]
    m = re.search(r"config=([\w.-]+)", r.get("notes") or "")
    if m:
        return m.group(1)
    if r.get("lb") == "packetbalance":
        return "packetbalance-%s" % (r.get("xdp_mode") or "?")
    return r.get("lb", "?")


def cfg_key(r):
    if r.get("config"):
        return r["config"]
    if r.get("lb") != "packetbalance":
        return r.get("lb", "?")
    s = "packetbalance"
    if r.get("hash") == "modulo":
        s += "-modulo"
    if r.get("conntrack") is False:
        s += "-noct"
    return s


def exp4_key(r):
    s = cfg_key(r)
    if r.get("ipvs_sloppy_tcp") is not None:
        s += "/sloppy%d" % r["ipvs_sloppy_tcp"]
    return s + ("/drift" if r.get("drift") else "")


def exp6_key(r):
    return "off" if r.get("conntrack") is False else str(r.get("conntrack_size"))


def grouped(name, key, ok):
    rows = rt.load(name)
    g = rt.group(rows, key)
    return ({k: [r for r in rs if ok(r)] for k, rs in g.items()},
            {k: [r for r in rs if not ok(r)] for k, rs in g.items()})


def field_any(rows, pattern):
    """First numeric value of a key matching pattern, searched one level into dicts."""
    out = []
    for r in rows:
        for k, v in r.items():
            if isinstance(v, dict):
                for k2, v2 in v.items():
                    if re.search(pattern, k2) and isinstance(v2, (int, float)):
                        out.append(v2)
            elif re.search(pattern, k) and isinstance(v, (int, float)) and not isinstance(v, bool):
                out.append(v)
    return out


# ---------------------------------------------------------------------------
# computing every headline number
# ---------------------------------------------------------------------------
EXP1_CFGS = ["packetbalance-native", "packetbalance-generic", "ipvs-mh", "ipvs-mh-tun", "ipvs-rr", "none"]
EXP2_CFGS = ["packetbalance", "ipvs-mh", "ipvs-mh-tun", "ipvs-rr", "none"]
EXP3_CFGS = ["packetbalance", "packetbalance-noct", "packetbalance-modulo", "ipvs-mh", "ipvs-rr"]
EXP4_CFGS = ["packetbalance", "packetbalance-modulo", "ipvs-mh/sloppy1", "ipvs-rr/sloppy1", "ipvs-mh/sloppy0",
             "ipvs-rr/sloppy0", "packetbalance/drift", "packetbalance-modulo/drift", "ipvs-mh/sloppy1/drift"]
EXP6_CFGS = ["off", "65536", "1048576", "8388608"]


def compute(basis="forwarded"):
    Qs = collections.OrderedDict()
    failed = []  # (experiment, config, note)

    def add(q):
        Qs[q.key] = q
        return q

    ppc_field = "pps_per_core" if basis == "forwarded" else "pps_per_core_received"

    # Exp 1 -----------------------------------------------------------------
    src = "results/exp1_pps.jsonl"
    ok, bad = grouped("exp1_pps.jsonl", exp1_key, lambda r: r.get("received_pps") is not None)
    for c, rs in bad.items():
        for r in rs:
            failed.append(("exp1", c, r.get("notes", "")))
    for c in EXP1_CFGS:
        rs = ok.get(c, [])
        add(Q("exp1.%s.fwd" % c, "Exp 1 %s forwarded pps" % c, [r.get("forwarded_pps") for r in rs], rs, "rate", src))
        add(Q("exp1.%s.rx" % c, "Exp 1 %s received pps at the reals" % c, [r.get("received_pps") for r in rs], rs,
              "rate", src))
        # none has no forwarded counter; its pps_per_core is received based already
        f = "pps_per_core" if c == "none" else ppc_field
        add(Q("exp1.%s.ppc" % c, "Exp 1 %s pps per core (est., %s basis)" % (c, "received" if c == "none" else basis),
              [r.get(f) for r in rs], rs, "rate", src))
        add(Q("exp1.%s.ppc_rx" % c, "Exp 1 %s pps per core, received basis" % c,
              [r.get("pps_per_core_received", r.get("pps_per_core") if c == "none" else None) for r in rs],
              rs, "rate", src))
    for num, den in (("packetbalance-native", "ipvs-mh"), ("packetbalance-generic", "ipvs-mh"),
                     ("packetbalance-native", "ipvs-mh-tun")):
        for what, label in (("rx", "received pps"), ("ppc", "pps per core")):
            a, b = Qs["exp1.%s.%s" % (num, what)], Qs["exp1.%s.%s" % (den, what)]
            vals = [a.mean / b.mean] if not a.missing and not b.missing and b.mean else []
            q = add(Q("ratio.%s/%s.%s" % (num, den, what), "Ratio %s / %s on %s (means)" % (num, den, label),
                      vals, a.rows + b.rows, "ratio", src))
            q.n_parts = (a.n, b.n)
    bpf = field_any(ok.get("packetbalance-native", []), r"(bpf_)?ns_per_(pkt|packet)")
    add(Q("exp1.bpf_ns", "XDP program ns per packet (bpf_stats), native", bpf, ok.get("packetbalance-native", []),
          "ns", src, note="needs a bpf_ns_per_packet field on the Exp 1 native rows"))

    # Exp 2 -----------------------------------------------------------------
    src = "results/exp2_http.jsonl"
    ok, bad = grouped("exp2_http.jsonl", cfg_key, lambda r: bool(r.get("req_per_s")))
    for c, rs in bad.items():
        for r in rs:
            failed.append(("exp2", c, r.get("notes", "")))
    for c in EXP2_CFGS:
        rs = ok.get(c, [])
        for f, kind in (("p50_us", "us"), ("p99_us", "us"), ("req_per_s", "reqs")):
            add(Q("exp2.%s.%s" % (c, f), "Exp 2 %s %s" % (c, f), [r.get(f) for r in rs], rs, kind, src))
    for c in EXP2_CFGS[:-1]:
        for f in ("p50_us", "p99_us"):
            a, b = Qs["exp2.%s.%s" % (c, f)], Qs["exp2.none.%s" % f]
            vals = [a.mean - b.mean] if not a.missing and not b.missing else []
            q = add(Q("exp2.%s.added_%s" % (c, f), "Exp 2 %s added %s over none (means)" % (c, f), vals,
                      a.rows + b.rows, "us", src))
            q.parts = (a, b)

    # Exp 3 -----------------------------------------------------------------
    src = "results/exp3_churn.jsonl"
    ok, bad = grouped("exp3_churn.jsonl", cfg_key, lambda r: "broken" in r)
    for c, rs in bad.items():
        for r in rs:
            failed.append(("exp3", c, r.get("notes", "")))
    for c in EXP3_CFGS:
        rs = ok.get(c, [])
        tot = max([r.get("established") or 0 for r in rs] or [0]) or None
        add(Q("exp3.%s.broken" % c, "Exp 3 %s broken" % c, [r["broken"] for r in rs], rs, "count", src, tot))
        b3 = [(r.get("broken_by_backend") or {}).get("3", 0) for r in rs]
        add(Q("exp3.%s.broken_real3" % c, "Exp 3 %s broken that were on real3" % c, b3, rs, "count", src, tot))
        add(Q("exp3.%s.broken_else" % c, "Exp 3 %s broken elsewhere" % c,
              [r["broken"] - x for r, x in zip(rs, b3)], rs, "count", src, tot))
        add(Q("exp3.%s.on_real3" % c, "Exp 3 %s connections on real3 at ramp end (before removal at t=10 s)" % c,
              [(r.get("backends_at_start") or {}).get("3", 0) for r in rs], rs, "count", src, tot))
        add(Q("exp3.%s.connect_failed" % c, "Exp 3 %s connect failures" % c,
              [r.get("connect_failed", 0) for r in rs], rs, "count", src, tot))

    # Exp 4 -----------------------------------------------------------------
    src = "results/exp4_failover.jsonl"
    ok, bad = grouped("exp4_failover.jsonl", exp4_key, lambda r: "broken" in r)
    for c, rs in bad.items():
        for r in rs:
            failed.append(("exp4", c, r.get("notes", "")))
    for c in EXP4_CFGS:
        rs = ok.get(c, [])
        tot = max([r.get("established") or 0 for r in rs] or [0]) or None
        add(Q("exp4.%s.broken" % c, "Exp 4 %s broken" % c, [r["broken"] for r in rs], rs, "count", src, tot))
        add(Q("exp4.%s.survived" % c, "Exp 4 %s survived" % c, [r.get("survived") for r in rs], rs, "count", src,
              tot))
        mn = [r.get("min_fraction_moved") for r in rs if r.get("min_fraction_moved")]
        if c.endswith("/drift"):
            add(Q("exp4.%s.theory" % c, "Exp 4 %s theoretical minimum broken (1/(N+1) of established)" % c,
                  [m * (r.get("established") or 0) for m, r in zip(mn, rs)], rs, "count", src, tot))

    # Exp 5 -----------------------------------------------------------------
    src = "results/exp5_hash_quality.json"
    p5 = os.path.join(RES, "exp5_hash_quality.json")
    d5 = json.load(open(p5)) if os.path.exists(p5) else {}
    r5 = [{"date": d5.get("date"), "notes": "tuples %s, ring %s, seed %s" % (d5.get("tuples"), d5.get("ring_size"),
                                                                           d5.get("seed"))}] if d5 else []
    for mode in ("maglev", "modulo"):
        m = next((x for x in d5.get("modes", []) if x.get("mode") == mode), None)
        dev = [max(abs(s["observed"] - s["expected"]) for s in m["shares"])] if m else []
        add(Q("exp5.%s.max_share_dev" % mode, "Exp 5 %s largest |observed - expected| share" % mode,
              dev, r5, "frac", src))
        for i, ev in enumerate((m or {}).get("disruption", [])):
            tag = "remove" if ev["event"].startswith("remove") else "add"
            q = add(Q("exp5.%s.%s" % (mode, tag), "Exp 5 %s flows moved on '%s'" % (mode, ev["event"]),
                      [ev["flows_moved"]], r5, "frac", src))
            q.target, q.event = ev["target"], ev["event"]
        for tag in ("remove", "add"):
            if "exp5.%s.%s" % (mode, tag) not in Qs:
                add(Q("exp5.%s.%s" % (mode, tag), "Exp 5 %s flows moved on %s" % (mode, tag), [], [], "frac", src))

    # Exp 6 -----------------------------------------------------------------
    src = "results/exp6_conntrack.jsonl"
    ok, bad = grouped("exp6_conntrack.jsonl", exp6_key, lambda r: r.get("forwarded_pps") is not None)
    for c, rs in bad.items():
        for r in rs:
            failed.append(("exp6", c, (r.get("notes") or "")[:160]))
    for c in EXP6_CFGS:
        rs = ok.get(c, [])
        add(Q("exp6.%s.ppc" % c, "Exp 6 conntrack %s pps per core (est., %s basis)" %
              ("off" if c == "off" else format(int(c), ",") + " entries", basis),
              [r.get(ppc_field) for r in rs], rs, "rate", src))
        add(Q("exp6.%s.fwd" % c, "Exp 6 conntrack %s forwarded pps" % c, [r.get("forwarded_pps") for r in rs], rs,
              "rate", src))
    a, b = Qs["exp6.off.ppc"], Qs["exp6.1048576.ppc"]
    q = add(Q("exp6.off_vs_1m", "Exp 6 pps per core, conntrack off / 1M table (means)",
              [a.mean / b.mean] if not a.missing and not b.missing and b.mean else [], a.rows + b.rows, "ratio", src))
    q.overlap = (not a.missing and not b.missing and a.lo <= b.hi and b.lo <= a.hi)
    mem = field_any(ok.get("1048576", []) + rt.load("exp1_pps.jsonl"), r"memlock")
    add(Q("ct_memlock_1m", "conntrack map memlock bytes at 1M entries (bpftool map show)", mem[:1],
          ok.get("1048576", []), "bytes", "results/exp6_conntrack.jsonl or exp1_pps.jsonl",
          note="needs a *memlock* field on a 1M-entry row"))

    # restart ---------------------------------------------------------------
    src = "results/restart.jsonl"
    ok, bad = grouped("restart.jsonl", lambda r: "restart", lambda r: "broken" in r)
    rs = ok.get("restart", [])
    for r in bad.get("restart", []):
        failed.append(("restart", "packetbalance", r.get("notes", "")))
    q = add(Q("restart.broken", "Daemon restart, broken connections per run",
              [r["broken"] for r in rs], rs, "count", src, max([r.get("established") or 0 for r in rs] or [0]) or None))
    q.per_run = [r["broken"] for r in rs]
    return Qs, failed


# ---------------------------------------------------------------------------
# repo facts (tests, bugs, CI)
# ---------------------------------------------------------------------------
def count_tests():
    tests_dir = os.path.join(ROOT, "tests")
    plain, params, inst = 0, collections.Counter(), {}
    for dp, _, fs in os.walk(tests_dir):
        for f in fs:
            if not f.endswith((".cpp", ".cc")):
                continue
            s = open(os.path.join(dp, f)).read()
            plain += len(re.findall(r"^\s*TEST(?:_F)?\(", s, re.M))
            for m in re.finditer(r"^\s*TEST_P\((\w+)", s, re.M):
                params[m.group(1)] += 1
            for m in re.finditer(r"INSTANTIATE_TEST_SUITE_P\(\s*\w+\s*,\s*(\w+)\s*,\s*::testing::Values\(([^)]*)\)", s):
                inst[m.group(1)] = len([x for x in m.group(2).split(",") if x.strip()])
    expanded = sum(n * inst.get(s, 1) for s, n in params.items())
    return plain + expanded, plain + sum(params.values())


def count_bugs():
    p = os.path.join(ROOT, "docs", "BUGS.md")
    return len(re.findall(r"^\| (\d+) \|", open(p).read(), re.M)) if os.path.exists(p) else None


def ci_status():
    try:
        rid = subprocess.run(["gh", "run", "list", "--workflow", "ci.yml", "--branch", "main", "-L", "1", "--json",
                              "databaseId,headSha,conclusion,createdAt"], cwd=ROOT, capture_output=True, text=True,
                             timeout=20)
        run = json.loads(rid.stdout)[0]
        jobs = subprocess.run(["gh", "run", "view", str(run["databaseId"]), "--json", "jobs"], cwd=ROOT,
                              capture_output=True, text=True, timeout=20)
        jobs = json.loads(jobs.stdout)["jobs"]
        e2e = next((j for j in jobs if j["name"] == "e2e"), None)
        skipped = [s["name"] for s in (e2e or {}).get("steps", []) if s.get("conclusion") == "skipped"
                   and "failure" not in s["name"]]
        return {"sha": run["headSha"][:7], "date": run["createdAt"][:10], "conclusion": run["conclusion"],
                "jobs": {j["name"]: j["conclusion"] for j in jobs}, "e2e_skipped": skipped}
    except Exception as e:  # offline, gh missing, not logged in
        return {"error": str(e) or type(e).__name__}


# ---------------------------------------------------------------------------
# formatting
# ---------------------------------------------------------------------------
def fcount(v):
    return format(int(round(v)), ",")


def fmt(q, style="doc", pct_of_total=False):
    """Format a quantity for a document. style doc: Mpps; style prose: million."""
    if q.missing:
        return None
    v = q.mean
    if q.kind == "rate":
        return ("%.2f Mpps" if style == "doc" else "%.2f million") % (v / 1e6)
    if q.kind == "ratio":
        return "%.2fx" % v
    if q.kind == "us":
        return "%.0f us" % v
    if q.kind == "reqs":
        return fcount(v)
    if q.kind == "frac":
        return "%.1f%%" % (100 * v)
    if q.kind in ("bytes", "ns"):
        return fcount(v) if q.kind == "bytes" else "%.0f" % v
    # count
    s = fcount(v)
    if q.n > 1 and q.hi != q.lo:
        s = "about %s (%s to %s across %d runs)" % (s, fcount(q.lo), fcount(q.hi), q.n)
    if pct_of_total and q.total:
        s += " of %s (%.1f%%)" % (fcount(q.total), 100.0 * v / q.total)
    return s


def show(q):
    if q.missing:
        return "MISSING"
    if q.kind == "rate":
        f = lambda v: "%.3f Mpps" % (v / 1e6)
    elif q.kind == "ratio":
        f = lambda v: "%.3fx" % v
    elif q.kind == "us":
        f = lambda v: "%.0f us" % v
    elif q.kind == "frac":
        f = lambda v: "%.2f%%" % (100 * v)
    else:
        f = lambda v: fcount(v)
    s = f(q.mean)
    if q.n > 1:
        s += " (%s to %s, spread %.1f%%)" % (f(q.lo), f(q.hi), q.spread_pct)
    return s + ", n=%d" % q.n


# ---------------------------------------------------------------------------
# placeholders
# ---------------------------------------------------------------------------
PLACEHOLDERS = collections.OrderedDict([
    ("EXP1_PPS_PER_CORE_NATIVE", ("exp1.packetbalance-native.ppc", {})),
    ("EXP1_PPS_PER_CORE_GENERIC", ("exp1.packetbalance-generic.ppc", {})),
    ("IPVS_MH_PPS_PER_CORE", ("exp1.ipvs-mh.ppc", {})),
    ("EXP1_BPF_NS_PER_PACKET", ("exp1.bpf_ns", {})),
    ("CT_MEMLOCK_BYTES_1M", ("ct_memlock_1m", {})),
    ("EXP6_PPS_PER_CORE_64K", ("exp6.65536.ppc", {})),
    ("EXP6_PPS_PER_CORE_1M", ("exp6.1048576.ppc", {})),
    ("EXP6_PPS_PER_CORE_8M", ("exp6.8388608.ppc", {})),
    ("EXP6_PPS_PER_CORE_NO_CONNTRACK", ("exp6.off.ppc", {})),
    ("EXP3_BROKEN_DEFAULT", ("exp3.packetbalance.broken", {})),
    ("EXP3_CONNS_ON_REAL3", ("exp3.packetbalance.on_real3", {})),
    ("EXP3_BROKEN_NO_CONNTRACK", ("exp3.packetbalance-noct.broken", {})),
    ("EXP3_BROKEN_MODULO", ("exp3.packetbalance-modulo.broken", {})),
    ("EXP4_BROKEN_PB", ("exp4.packetbalance.broken", {})),
    ("EXP4_BROKEN_IPVS_MH", ("exp4.ipvs-mh/sloppy1.broken", {})),
    ("EXP4_BROKEN_IPVS_RR", ("exp4.ipvs-rr/sloppy1.broken", {})),
    ("EXP4_DRIFT_BROKEN_MAGLEV", ("exp4.packetbalance/drift.broken", {"pct_of_total": True})),
    ("EXP4_DRIFT_BROKEN_MODULO", ("exp4.packetbalance-modulo/drift.broken", {"pct_of_total": True})),
    ("RESTART_BROKEN_CONNS", ("restart.broken", {"pct_of_total": False, "of_total": True})),
])


def placeholder_value(name, Qs, style):
    key, opts = PLACEHOLDERS[name]
    q = Qs.get(key)
    if q is None or q.missing:
        return None, q
    s = fmt(q, style, pct_of_total=opts.get("pct_of_total", False))
    if opts.get("of_total") and q.total:
        if q.n > 1 and q.hi == q.lo:
            s += " of %s (all %d runs)" % (fcount(q.total), q.n)
        else:
            s += " of %s" % fcount(q.total)
    return s, q


def targets(defense):
    return [(os.path.join(TEMPLATES, "CAPACITY.md"), os.path.join(ROOT, "docs", "CAPACITY.md"), "doc"),
            (os.path.join(os.path.dirname(defense), "templates", os.path.basename(defense)),
             defense, "prose")]


def do_fill(Qs, defense, dry_dir=None):
    rc = 0
    for tpl, dst, style in targets(defense):
        if dry_dir:
            os.makedirs(dry_dir, exist_ok=True)
            dst = os.path.join(dry_dir, os.path.basename(dst))
        if not os.path.exists(tpl):
            print("template missing: %s (copy the placeholder version there first)" % tpl, file=sys.stderr)
            rc = 1
            continue
        text = open(tpl).read()
        seen = set()

        def say(msg):
            if msg not in seen:
                seen.add(msg)
                print(msg)

        def sub(m):
            name = m.group(1)
            if name not in PLACEHOLDERS:
                say("WARN %s: {{%s}} has no mapping; left in place" % (os.path.basename(dst), name))
                return m.group(0)
            val, q = placeholder_value(name, Qs, style)
            if val is None:
                say("WARN %s: {{%s}} not filled, %s is MISSING%s" % (
                    os.path.basename(dst), name, PLACEHOLDERS[name][0], (" (" + q.note + ")") if q and q.note else ""))
                return m.group(0)
            if not q.verified():
                say("NOTE %s: {{%s}} = %s is not 'verified' (n=%d%s)" % (
                    os.path.basename(dst), name, val, q.n, ", " + "; ".join(q.flags) if q.flags else ""))
            return val

        out = re.sub(r"\{\{([A-Z0-9_]+)\}\}", sub, text)
        old = open(dst).read() if os.path.exists(dst) else None
        if out != old:
            open(dst, "w").write(out)
            print("wrote %s" % dst)
        else:
            print("unchanged %s" % dst)
    if dry_dir:
        print("dry run: README.md not touched (render_tables.py --write skipped)")
        return rc
    r = subprocess.run([sys.executable, os.path.join(ROOT, "lab", "render_tables.py"), "--write"],
                       cwd=ROOT, stdout=subprocess.DEVNULL)
    return rc or r.returncode


# ---------------------------------------------------------------------------
# plausibility checks
# ---------------------------------------------------------------------------
def run_checks(Qs, failed):
    """Attach flags to quantities, return general warnings."""
    warn = []
    g = lambda k: Qs[k]

    for q in Qs.values():
        if q.missing:
            continue
        if q.kind in ("rate", "us", "reqs") and q.n > 1 and q.spread_pct > SPREAD_LIMIT:
            q.flags.append("spread %.1f%% > %.0f%%" % (q.spread_pct, SPREAD_LIMIT))
        if q.kind == "count" and not q.consistent():
            q.flags.append("repeats disagree (%s to %s)" % (fcount(q.lo), fcount(q.hi)))
        if q.kind not in ("ratio", "frac") and q.n < 3:
            q.flags.append("n=%d < 3 repeats" % q.n)
        if q.n > 3 and q.kind not in ("ratio",):
            q.flags.append("n=%d > 3, reruns appended? check for rows to supersede" % q.n)
        commits = {commit_of(r) for r in q.rows} if q.src.endswith(".jsonl") else set()
        if len(commits) > 1:
            q.flags.append("rows span %d commits (%s)" % (len(commits), "; ".join(sorted(commits))))
        elif "commit not recorded" in commits:
            q.flags.append("commit not recorded in notes")
        gates = {r.get("host_gate") for r in q.rows if "host_gate" in r}
        if gates - {"pass"}:
            q.flags.append("host_gate %s" % gates)

    # Exp 1
    none_rx = g("exp1.none.rx")
    for c in EXP1_CFGS[:-1]:
        fwd, rx = g("exp1.%s.fwd" % c), g("exp1.%s.rx" % c)
        if not none_rx.missing and not rx.missing:
            ceiling = none_rx.mean + (none_rx.hi - none_rx.lo)
            if rx.mean > ceiling:
                rx.flags.append("received %.2f Mpps above the no-LB ceiling %.2f Mpps (+spread)" %
                                (rx.mean / 1e6, ceiling / 1e6))
                g("exp1.%s.ppc" % c).flags.append("rate above the no-LB ceiling")
        if not fwd.missing and not rx.missing and fwd.mean and rx.mean < 0.9 * fwd.mean:
            drops = statistics.mean([r.get("reals_rx_dropped_pps") or 0 for r in rx.rows])
            msg = ("only %.0f%% of forwarded packets reached the reals (fwd %.2f, rx %.2f Mpps, reals rx_dropped "
                   "%.2f Mpps)" % (100 * rx.mean / fwd.mean, fwd.mean / 1e6, rx.mean / 1e6, drops / 1e6))
            g("exp1.%s.ppc" % c).flags.append(msg)
            if drops < 0.5 * (fwd.mean - rx.mean):
                g("exp1.%s.ppc" % c).flags.append("most of that loss is not the reals' rx_dropped, so packets "
                                                  "counted as forwarded vanished between the LB and the reals")
    if none_rx.missing:
        warn.append("Exp 1 has no 'none' rows, so there is no ceiling to check the LB rates against")
    nat, gen = g("exp1.packetbalance-native.ppc"), g("exp1.packetbalance-generic.ppc")
    if not nat.missing and not gen.missing and gen.mean > nat.mean:
        nat.flags.append("generic (%.2f Mpps/core) beats native (%.2f), unexpected; find out why before quoting"
                         % (gen.mean / 1e6, nat.mean / 1e6))
    for k in ("ratio.packetbalance-native/ipvs-mh.rx", "ratio.packetbalance-native/ipvs-mh.ppc"):
        q = g(k)
        if not q.missing and q.mean < 1:
            q.flags.append("PacketBalance native is below IPVS mh (%.2fx); publish it, and bullet 1 cannot say "
                           "'X times IPVS'" % q.mean)
    for q in Qs.values():
        if q.key.startswith("ratio."):
            a, b = q.n_parts
            if not q.missing and (a < 3 or b < 3):
                q.flags.append("built from n=%d and n=%d repeats" % (a, b))
    if g("exp1.bpf_ns").missing:
        warn.append("Exp 1 rows carry no bpf_stats field, so EXP1_BPF_NS_PER_PACKET cannot be filled from rows")

    # Exp 2
    for c in EXP2_CFGS[:-1]:
        for f in ("p50_us", "p99_us"):
            q = g("exp2.%s.added_%s" % (c, f))
            if q.missing:
                continue
            a, b = q.parts
            if q.mean < 0 and a.hi < b.lo:
                q.flags.append("LB faster than direct (%.0f us), check the none row (one nginx, not four)" % q.mean)
            if a.n < 3 or b.n < 3:
                q.flags.append("built from n=%d and n=%d repeats" % (a.n, b.n))
            if (a.flags or b.flags) and "inputs flagged" not in q.flags:
                q.flags.append("inputs flagged")
    if not g("exp2.none.p50_us").missing:
        warn.append("Exp 2 'none' is direct to real1 (one nginx, not four); req/s are not comparable, latency is")

    # Exp 3
    for c in EXP3_CFGS:
        q, on3, b3, other = (g("exp3.%s.%s" % (c, k)) for k in ("broken", "on_real3", "broken_real3", "broken_else"))
        if q.missing:
            continue
        if c in ("packetbalance", "ipvs-mh", "ipvs-rr") and other.mean > 0.01 * (q.total or 10000):
            q.flags.append("%s broken on reals other than real3 (expected ~0)" % fcount(other.mean))
        if c in ("packetbalance", "ipvs-mh", "ipvs-rr") and abs(b3.mean - on3.mean) > 0.02 * (q.total or 10000):
            q.flags.append("broken on real3 %s vs %s on real3 at start" % (fcount(b3.mean), fcount(on3.mean)))
        if g("exp3.%s.connect_failed" % c).mean:
            q.flags.append("connect failures %s" % fcount(g("exp3.%s.connect_failed" % c).mean))
    m = g("exp3.packetbalance-modulo.broken")
    if not m.missing and m.lo == 0:
        m.flags.append("zero broken in a modulo churn row, implausible (a modulo ring moves most flows)")
    d, n = g("exp3.packetbalance.broken"), g("exp3.packetbalance-noct.broken")
    if not d.missing and not n.missing and n.mean < d.mean:
        n.flags.append("no-conntrack broke fewer than default, unexpected")

    # Exp 4
    pb = g("exp4.packetbalance.broken")
    if not pb.missing and pb.mean > 0.01 * (pb.total or 10000):
        pb.flags.append("PacketBalance failover broke %.1f%%, expected near zero" % (100 * pb.mean / pb.total))
    rr = g("exp4.ipvs-rr/sloppy1.broken")
    if not rr.missing and rr.mean < 0.5 * (rr.total or 10000):
        rr.flags.append("IPVS rr broke under half, expected about three quarters")
    for c in ("packetbalance/drift", "packetbalance-modulo/drift", "ipvs-mh/sloppy1/drift"):
        q, th = g("exp4.%s.broken" % c), g("exp4.%s.theory" % c)
        if q.missing or th.missing:
            continue
        frac = q.mean / (q.total or 10000)
        if "modulo" not in c and not 0.5 * th.mean <= q.mean <= 1.75 * th.mean:
            q.flags.append("drift broke %.1f%%, far from the 1/(N+1) = %.0f%% minimum" % (100 * frac, 100 * th.mean
                                                                                          / (q.total or 10000)))
    mg, md = g("exp4.packetbalance/drift.broken"), g("exp4.packetbalance-modulo/drift.broken")
    if not mg.missing and not md.missing and md.mean <= mg.mean:
        md.flags.append("modulo drift broke no more than Maglev drift, implausible")

    # Exp 5
    for mode in ("maglev", "modulo"):
        dev = g("exp5.%s.max_share_dev" % mode)
        if not dev.missing and dev.mean > 0.01:
            dev.flags.append("a real's share is off by %.2f points" % (100 * dev.mean))
        for tag in ("remove", "add"):
            q = g("exp5.%s.%s" % (mode, tag))
            if q.missing:
                continue
            if mode == "maglev" and abs(q.mean - q.target) > 0.02:
                q.flags.append("Maglev moved %.1f%% vs minimum %.1f%%" % (100 * q.mean, 100 * q.target))
            if mode == "modulo" and q.mean < 2 * q.target:
                q.flags.append("modulo moved only %.1f%%, expected most flows" % (100 * q.mean))

    # Exp 6
    q = g("exp6.off_vs_1m")
    if not q.missing and q.overlap:
        q.note = "ranges overlap, no difference within the spread"

    # restart
    q = g("restart.broken")
    if not q.missing:
        if q.hi > 0:
            q.flags.append("connections broke across a restart (per run %s)" % q.per_run)
        for r in q.rows:
            if r.get("flows_tracked_before") != r.get("flows_tracked_after_restart"):
                q.flags.append("flows tracked %s before vs %s after restart (repeat %s)" % (
                    r.get("flows_tracked_before"), r.get("flows_tracked_after_restart"), r.get("repeat")))

    for e, c, note in failed:
        warn.append("%s %s: failed row, note '%s'" % (e, c, (note or "")[:140]))
    rej = rt.load("rejected.jsonl")
    if rej:
        warn.append("results/rejected.jsonl holds %d host-gate rejected windows (not used)" % len(rej))
    return warn


def do_check(Qs, warn, basis):
    sections = [("Experiment 1: packet rate (64-byte UDP, 10,000 flows, pps per core basis %s)" % basis, "exp1."),
                ("Ratios", "ratio."), ("Experiment 2: wrk latency and throughput", "exp2."),
                ("Experiment 3: backend churn", "exp3."), ("Experiment 4: LB failover", "exp4."),
                ("Experiment 5: hash quality", "exp5."), ("Experiment 6: connection table cost", "exp6."),
                ("Connection table memory", "ct_"), ("Daemon restart", "restart.")]
    nflag = 0
    for title, pre in sections:
        print("== %s" % title)
        last = None
        for q in Qs.values():
            if not q.key.startswith(pre) or q.key.endswith(".connect_failed"):
                continue
            p = prov(q.rows)
            if not q.missing and p and pre not in ("ratio.",) and (p.get("host"), p.get("n"), p.get("commit")) != last:
                print("   [%s: %s]" % (q.src, prov_line(q)))
                last = (p.get("host"), p.get("n"), p.get("commit"))
            extra = ""
            if getattr(q, "target", None) is not None:
                extra = " (minimum possible %.2f%%)" % (100 * q.target)
            if q.note and not q.missing and q.key == "exp6.off_vs_1m":
                extra = " (%s)" % q.note
            print("  %-4s %-70s %s%s" % ("MISS" if q.missing else ("ok" if not q.flags else "FLAG"), q.desc,
                                          show(q), extra))
            for f in q.flags:
                print("         ! %s" % f)
                nflag += 1
    print("== Placeholders")
    for name, (key, _) in PLACEHOLDERS.items():
        v, q = placeholder_value(name, Qs, "doc")
        print("  %-32s <- %-40s %s" % (name, key, v if v else "MISSING" + (" (" + q.note + ")" if q and q.note
                                                                                  else "")))
    if warn:
        print("== Warnings")
        for w in warn:
            print("  ! %s" % w)
    missing = sum(1 for q in Qs.values() if q.missing)
    print("\n%d quantities, %d MISSING, %d flags, %d warnings" % (len(Qs), missing, nflag, len(warn)))


# ---------------------------------------------------------------------------
# ledger
# ---------------------------------------------------------------------------
def lab_prov(q, extra=""):
    p = prov(q.rows)
    if not p:
        return "No rows yet in `%s`" % q.src
    s = ("Lab in a VM, not a physical NIC or production traffic. %s, %s vCPUs, kernel %s (%s)" %
         (p["host"], p["cpus"], p["kernel"], p["arch"]))
    if p["xdp_mode"] != "n/a":
        s += ", XDP %s" % p["xdp_mode"]
    if p["pkt_size"] != "n/a":
        s += ", %s-byte UDP frames" % p["pkt_size"]
    if p["flows"] != "n/a":
        s += ", %s flows" % p["flows"]
    if p["conntrack_size"] != "n/a":
        s += ", conntrack %s entries" % p["conntrack_size"]
    s += ". n=%s, %s, %s. `%s`" % (p["n"], p["date"], p["commit"], q.src)
    if extra:
        s += ". " + extra
    return s


def status(qs, context):
    qs = [q for q in qs if q is not None]
    if not qs or any(q.missing for q in qs):
        return "**not measured**", "Rows MISSING (%s)" % ", ".join(q.key for q in qs if q.missing)
    flags = [f for q in qs for f in q.flags]
    if all(q.verified() for q in qs) and not context:
        return "**verified**", ""
    ctx = "; ".join(filter(None, [context] + flags)) or "n=%s" % "/".join(str(q.n) for q in qs)
    return "**verified, needs context**", "Needs context: " + ctx


def ledger_rows(Qs, basis):
    g = Qs.get
    rows = []

    def row(claim, qs, prov_text, context=""):
        st, ctx = status(qs, context)
        if st == "**not measured**":
            rows.append("| %s | %s | %s. %s |" % (claim, st, ctx, "`%s`" % qs[0].src if qs and qs[0] else ""))
        else:
            rows.append("| %s | %s | %s%s |" % (claim, st, prov_text, (". " + ctx) if ctx else ""))

    lb = ("pps per core = forwarded pps / (VM busy fraction x CPUs), charging the generator, bridge and reals "
          "to the LB, so a lower bound and fair only between rows on the same VM. On a veth native XDP still "
          "starts from an skb the sender built")
    for c, name in (("packetbalance-native", "native"), ("packetbalance-generic", "generic")):
        q, rx = g("exp1.%s.ppc" % c), g("exp1.%s.ppc_rx" % c)
        claim = "PacketBalance %s XDP forwards **%s** per core" % (name, fmt(q, "doc") if not q.missing else "N")
        extra = lb + " (basis %s)" % basis
        if not q.missing and not rx.missing and abs(rx.mean - q.mean) > 0.1 * q.mean:
            extra += ". Delivered to the reals it is %.2f Mpps per core; quote the smaller" % (rx.mean / 1e6)
        row(claim, [q], lab_prov(q, extra), "a VM estimate, a lower bound")

    for what, label in (("ppc", "pps per core"), ("rx", "received pps")):
        q = g("ratio.packetbalance-native/ipvs-mh.%s" % what)
        a, b = g("exp1.packetbalance-native.%s" % what), g("exp1.ipvs-mh.%s" % what)
        claim = "PacketBalance native **%s** Linux IPVS `mh` (DR) on %s" % (fmt(q) if not q.missing else "X", label)
        tun = g("ratio.packetbalance-native/ipvs-mh-tun.%s" % what)
        extra = ("Ratio of means, %s against %s. IPVS DR only rewrites the MAC, PacketBalance encapsulates, "
                 "so the DR comparison favours IPVS; against IPVS `mh` over IPIP (like for like) it is %s" %
                 (show(a), show(b), fmt(tun) if not tun.missing else "MISSING"))
        row(claim, [q, a, b], lab_prov(a, extra), "same VM, same load, never compare with Katran's published numbers")

    a, b = g("exp2.packetbalance.added_p50_us"), g("exp2.packetbalance.added_p99_us")
    claim = "PacketBalance adds **%s** at p50 and **%s** at p99 over no load balancer" % (
        fmt(a) if not a.missing else "N", fmt(b) if not b.missing else "N")
    if a.missing:
        extra = ""
    else:
        extra = ("`wrk -t4 -c256 -d30s --latency` against nginx. p50 %s vs %s direct, p99 %s vs %s. IPVS mh adds %s "
                 "and %s. The direct row is one nginx, not four, and client, LB and reals share the VM's CPUs" %
                 (show(a.parts[0]), show(a.parts[1]), show(b.parts[0]), show(b.parts[1]),
                  fmt(g("exp2.ipvs-mh.added_p50_us")) or "MISSING", fmt(g("exp2.ipvs-mh.added_p99_us")) or "MISSING"))
    row(claim, [a, b], lab_prov(a.parts[0] if not a.missing else a, extra), "shared CPUs, a lab latency")

    names = {"packetbalance": "PacketBalance (Maglev + conntrack)", "packetbalance-noct": "PacketBalance, no conntrack",
             "packetbalance-modulo": "PacketBalance, modulo ring + conntrack", "ipvs-mh": "IPVS `mh`",
             "ipvs-rr": "IPVS `rr`"}
    for c in EXP3_CFGS:
        q, on3, oth = g("exp3.%s.broken" % c), g("exp3.%s.on_real3" % c), g("exp3.%s.broken_else" % c)
        claim = "Backend removal broke **%s** of **10,000** connections under %s" % (
            fcount(q.mean) if not q.missing else "N", names[c])
        extra = ("" if q.missing else "Mean over repeats, range %s to %s. %s were on real3 when it was removed at "
                 "t=10 s and %s broke on other reals. real3 was re-added at t=20 s" %
                 (fcount(q.lo), fcount(q.hi), fcount(on3.mean), fcount(oth.mean)))
        row(claim, [q], lab_prov(q, extra), "")

    labels4 = {"packetbalance": "PacketBalance", "ipvs-mh/sloppy1": "IPVS `mh` (sloppy_tcp=1)",
               "ipvs-rr/sloppy1": "IPVS `rr` (sloppy_tcp=1)", "ipvs-mh/sloppy0": "IPVS `mh` (sloppy_tcp=0)",
               "ipvs-rr/sloppy0": "IPVS `rr` (sloppy_tcp=0)", "packetbalance-modulo": "PacketBalance modulo ring"}
    for c, lab in labels4.items():
        q = g("exp4.%s.survived" % c)
        claim = "**%s** of **10,000** live TCP connections survived a load balancer swap under %s" % (
            fcount(q.mean) if not q.missing else "N", lab)
        extra = "" if q.missing else "The swap is the client's route flipping to lb2, which has an empty connection table"
        ctx = ""
        if c.startswith("ipvs-mh/sloppy1"):
            ctx = "IPVS `mh` matching PacketBalance is the expected result, it is Maglev in the kernel. Say so first"
        if "sloppy0" in c:
            ctx = "sloppy_tcp=0 is the IPVS default; a director that never saw the SYN drops the flow (BUGS.md #14)"
        row(claim, [q], lab_prov(q, extra), ctx)
    for c, lab in (("packetbalance/drift", "Maglev"), ("packetbalance-modulo/drift", "modulo"),
                   ("ipvs-mh/sloppy1/drift", "IPVS `mh`")):
        q, th = g("exp4.%s.broken" % c), g("exp4.%s.theory" % c)
        claim = "With one extra real on the new LB only, **%s** of **10,000** broke under %s against a **%s** minimum" % (
            fcount(q.mean) if not q.missing else "N", lab, fcount(th.mean) if not th.missing else "2,000")
        row(claim, [q], lab_prov(q, "Minimum is 1/(N+1) = 20% of connections for a fifth real"), "")

    for mode in ("maglev", "modulo"):
        rm, ad = g("exp5.%s.remove" % mode), g("exp5.%s.add" % mode)
        claim = "%s moves **%s** of flows when one of four reals is removed and **%s** when one is added" % (
            mode.capitalize(), fmt(rm) or "N", fmt(ad) or "N")
        dev = g("exp5.%s.max_share_dev" % mode)
        p = ("Control plane test tool, 1,000,000 synthetic 5-tuples, ring 65,537, weights 1:1:2:4, machine "
             "independent. Minimum possible %.1f%% and %.1f%%. Largest share error %.2f points. `%s`" % (
                 100 * getattr(rm, "target", float("nan")), 100 * getattr(ad, "target", float("nan")),
                 100 * (dev.mean or 0), rm.src)) if not rm.missing else ""
        row(claim, [rm, ad], p, "synthetic tuples, not traffic" if mode == "maglev" else "")

    off, one, r = g("exp6.off.ppc"), g("exp6.1048576.ppc"), g("exp6.off_vs_1m")
    if not r.missing:
        verdict = ("no measurable difference" if r.overlap else
                   "%.0f%% %s with conntrack off" % (abs(r.mean - 1) * 100, "faster" if r.mean > 1 else "slower"))
    else:
        verdict = "N"
    claim = "Connection table cost at 10,000 flows, **%s** pps per core off vs **%s** with 1M entries, %s" % (
        fmt(off) or "N", fmt(one) or "N", verdict)
    sizes = ", ".join("%s %s" % (k, fmt(g("exp6.%s.ppc" % k)) or "MISSING") for k in EXP6_CFGS)
    row(claim, [off, one], lab_prov(one, "All sizes %s. 10,000 flows fit in cache at every size, so a flat "
                                         "result cannot show the large-table cost" % sizes),
        "a VM estimate, the same caveats as Exp 1")

    q = g("restart.broken")
    claim = "**%s** of **%s** live connections broke across a daemon restart" % (
        fcount(q.mean) if not q.missing else "N", fcount(q.total) if q.total else "1,000")
    row(claim, [q], lab_prov(q, "SIGTERM at t=10 s, started again at t=15 s, XDP program and pinned maps outlive "
                                "the process. Per run %s. This is the harness's kill and start, not `systemctl "
                                "restart`" % getattr(q, "per_run", [])), "")

    tests, defs = count_tests()
    rows.append("| **%d** tests | **verified, needs context** | Counted statically from `tests/`: %d TEST/TEST_F/"
                "TEST_P definitions, TEST_P expanded over their instantiation values. Google Test for the core "
                "and daemon, `BPF_PROG_TEST_RUN` for the data plane (needs root). Rerun `ctest --test-dir build` "
                "for the live count |" % (tests, defs))
    bugs = count_bugs()
    rows.append("| **%s** real bugs logged with how each was found | **verified** | `docs/BUGS.md` index, each with "
                "symptom, tool and fix. Every entry was hit, none hypothetical |" % (bugs if bugs is not None else "N"))
    ci = ci_status()
    if "error" in ci:
        rows.append("| CI runs the namespaced lab end to end on every push | **unverified** | Could not query GitHub "
                    "Actions (%s). Check `gh run list --workflow ci.yml` |" % ci["error"][:80])
    else:
        e2e = ci["jobs"].get("e2e")
        skipped = ci["e2e_skipped"]
        st = "**verified**" if e2e == "success" and not skipped else "**verified, needs context**"
        rows.append("| CI runs the namespaced lab end to end on every push, curl through IPVS and PacketBalance | %s | "
                    "`.github/workflows/ci.yml` job `e2e` on a stock ubuntu-24.04 runner, latest run on main %s (%s) "
                    "%s, jobs %s.%s The runner is x86-64 with its own kernel; it proves the lab and forwarding work, "
                    "it measures nothing |" % (
                        st, ci["sha"], ci["date"], ci["conclusion"],
                        ", ".join("%s %s" % kv for kv in ci["jobs"].items()),
                        (" Steps skipped: %s." % ", ".join(skipped)) if skipped else " No e2e step skipped."))
    return rows


def do_ledger(Qs, basis):
    print("## PacketBalance\n")
    print("All from `~/Documents/PacketBalance/results/`, produced by `lab/experiments.sh` in the Lima lab VM. "
          "A lab in a VM, never a physical NIC or production traffic, never compared with Katran's numbers.\n")
    print("| Claim | Status | Provenance |")
    print("| --- | --- | --- |")
    for r in ledger_rows(Qs, basis):
        print(r)


# ---------------------------------------------------------------------------
# resume bullets
# ---------------------------------------------------------------------------
def b(x):
    return "**%s**" % x


def bullet_checks(text, used):
    out = []
    plain = text.replace("**", "")
    for ch, name in (("—", "em dash"), ("–", "en dash"), (":", "colon"), (";", "semicolon")):
        if ch in plain:
            out.append("contains a %s" % name)
    for m in re.finditer(r"\*\*(.+?)\*\*", text):
        if not re.search(r"\d", m.group(1)) and not re.fullmatch(r"[A-Z]", m.group(1)):
            out.append("bolds a non-number '%s'" % m.group(1))
    if len(plain) > 2 * LINE_CHARS:
        out.append("%d characters, likely over two lines (about %d per line); measure it in the resume build" %
                   (len(plain), LINE_CHARS))
    for q in used:
        if q is None or q.missing:
            out.append("uses a MISSING number (%s)" % (q.key if q else "?"))
        elif not q.verified():
            out.append("uses %s which is not 'verified' (%s)" % (q.key, "; ".join(q.flags) or "n=%d" % q.n))
    return out


def do_bullets(Qs, basis):
    g = Qs.get
    nat, ratio = g("exp1.packetbalance-native.ppc"), g("ratio.packetbalance-native/ipvs-mh.ppc")
    p = prov(nat.rows) if not nat.missing else {}
    vm = "with native XDP in a %s vCPU VM" % p.get("cpus", "N")
    N = "%.2fM" % (nat.mean / 1e6) if not nat.missing else "N"
    X = "%.1fx" % ratio.mean if not ratio.missing else "X"
    pb4, rr4 = g("exp4.packetbalance.survived"), g("exp4.ipvs-rr/sloppy1.survived")
    Y = fcount(pb4.mean) if not pb4.missing else "Y"
    Z = fcount(rr4.mean) if not rr4.missing else "Z"
    d3, m3 = g("exp3.packetbalance.broken"), g("exp3.packetbalance-modulo.broken")
    rs = g("restart.broken")

    bullets = []
    if not ratio.missing and ratio.mean < 1:
        b1 = ("Built an XDP/eBPF Layer 4 load balancer in C and C++ with Maglev hashing, per-CPU LRU connection "
              "tracking and IPIP direct server return, forwarding %s packets per second per core %s" % (b(N), vm))
        b1_note = "ratio vs IPVS mh is %.2fx (below 1), so the 'X times IPVS' clause is dropped" % ratio.mean
    else:
        b1 = ("Built an XDP/eBPF Layer 4 load balancer in C and C++ with Maglev hashing, per-CPU LRU connection "
              "tracking and IPIP direct server return, forwarding %s packets per second per core %s, %s the rate "
              "of Linux IPVS" % (b(N), vm, b(X)))
        b1_note = ""
    bullets.append(("Spec bullet 1", b1, [nat, ratio], b1_note))
    b2 = ("Kept %s of %s live TCP connections through a load balancer swap versus %s under IPVS round robin, "
          "measuring survival through backend churn and failover and writing the capacity model from the results"
          % (b(Y), b("10,000"), b(Z)))
    bullets.append(("Spec bullet 2, result first", b2, [pb4, rr4], ""))
    a1 = ("Held broken connections to %s of %s during a backend removal versus %s with a modulo ring, by pairing "
          "Maglev hashing with an LRU connection table in XDP" % (
              b(fcount(d3.mean) if not d3.missing else "N"), b("10,000"),
              b(fcount(m3.mean) if not m3.missing else "M")))
    bullets.append(("Alternate A, churn", a1, [d3, m3], ""))
    a2 = ("Restarted the load balancer daemon with %s of %s live connections broken, keeping the XDP program and "
          "connection table pinned in bpffs across the process exit" % (
              b(fcount(rs.mean) if not rs.missing else "N"), b(fcount(rs.total) if rs.total else "1,000")))
    bullets.append(("Alternate B, restart", a2, [rs], ""))

    for title, text, used, note in bullets:
        print("%s\n- %s" % (title, text))
        for w in bullet_checks(text, used) + ([note] if note else []):
            print("  WARNING %s" % w)
        print()
    print("Numbers come from results/ via lab/fill_numbers.py; each needs its NUMBERS_LEDGER.md row first "
          "(--ledger). pps per core basis: %s." % basis)


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--fill", action="store_true")
    ap.add_argument("--ledger", action="store_true")
    ap.add_argument("--bullets", action="store_true")
    ap.add_argument("--dry-run", metavar="DIR", help="with --fill, write the filled files into DIR and leave "
                    "docs/CAPACITY.md, the defense file and README.md alone")
    ap.add_argument("--basis", choices=("forwarded", "received"), default="forwarded")
    ap.add_argument("--defense", default=os.environ.get("PB_DEFENSE_DOC", DEFENSE_DEFAULT))
    a = ap.parse_args()
    if not (a.check or a.fill or a.ledger or a.bullets):
        a.check = True
    Qs, failed = compute(a.basis)
    warn = run_checks(Qs, failed)
    rc = 0
    if a.check:
        do_check(Qs, warn, a.basis)
    if a.fill:
        rc = do_fill(Qs, a.defense, a.dry_run)
    if a.ledger:
        do_ledger(Qs, a.basis)
    if a.bullets:
        do_bullets(Qs, a.basis)
    return rc


if __name__ == "__main__":
    sys.exit(main())
