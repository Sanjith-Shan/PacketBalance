# docs/templates/

The versions of the two documents that quote measured numbers, with the numbers still
as `{{PLACEHOLDER}}` tokens. `lab/fill_numbers.py --fill` always reads these and writes
the filled copies, so it can be rerun after new rows arrive and the old numbers are
replaced rather than stacked.

| Template | Filled copy |
|---|---|
| `CAPACITY.md` | `docs/CAPACITY.md` |
| `(the interview notes template lives outside the repo, next to the notes themselves)` | `~/Documents/MasterIntern/interview/meta/(the interview notes template lives outside the repo, next to the notes themselves)` (override with `--defense PATH` or `PB_DEFENSE_DOC`) |

Edit prose here, not in the filled copies. A fill overwrites the filled copy with the
template plus numbers, so an edit made only in the filled copy is lost on the next run.

## The flow

1. `sudo lab/experiments.sh all` in the lab VM appends rows to `results/*.jsonl`
   (and `tools/hashquality` writes `results/exp5_hash_quality.json`).
2. `python3 lab/fill_numbers.py --check` prints every headline number as mean, range and
   n, with its provenance (host, CPUs, kernel, XDP mode, packet size, flows, conntrack
   size, date, commit from the notes), and flags anything implausible: spread over 15%,
   fewer than three repeats, a missing configuration, a rate above the no-LB ceiling,
   forwarded packets that never reached the reals, zero broken in the modulo churn row,
   rows from more than one commit or with no commit recorded. It also lists which
   placeholder each number fills. Fix or rerun until the flags are ones you can explain.
   `--fill --dry-run DIR` shows the filled documents in DIR without touching anything.
3. `python3 lab/fill_numbers.py --fill` substitutes the placeholders into both documents
   and runs `lab/render_tables.py --write` to refresh the README tables. A placeholder
   with no rows stays as it is and is reported.
4. `python3 lab/fill_numbers.py --ledger` prints the `NUMBERS_LEDGER.md` rows (Claim,
   Status, Provenance). Status is **verified** only with three or more consistent
   repeats and no flags; otherwise **verified, needs context** with the context written
   out, or **not measured**. Paste them into the ledger by hand after reading them.
5. `python3 lab/fill_numbers.py --bullets` prints the two resume bullets from the spec
   and two alternates with the numbers in, and warns about any number that is not
   verified, any colon or dash, and a bullet likely to run past two lines.

`--basis received` switches every pps-per-core figure from forwarded packets to packets
that arrived at the reals. Use it when `--check` reports a large gap between the two.

The quantities that cannot come from the current rows are `EXP1_BPF_NS_PER_PACKET`
(needs a `bpf_ns_per_packet` field from `bpftool prog show` with
`kernel.bpf_stats_enabled=1`) and `CT_MEMLOCK_BYTES_1M` (needs a `*memlock*` field from
`bpftool map show` on a 1M-entry run). Until a row carries them they stay as placeholders.
