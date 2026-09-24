# PacketBalance control API

The daemon (`packetbalance`) listens on a Unix domain socket, default
`/run/packetbalance.sock`. The protocol is newline-delimited JSON. One request object
per line, one response object per line. `pbctl` is a thin client over this protocol
and the lab harness drives the daemon through it directly with `socat` or Python.

## Framing

Request

```json
{"cmd": "<command>", ...arguments...}
```

Response

```json
{"ok": true, "result": <anything>}
{"ok": false, "error": "<human readable message>"}
```

Every response is exactly one line. The daemon handles one request at a time per
connection and accepts many connections.

## Addressing

A VIP is written `ADDR:PORT/PROTO`, for example `198.51.100.1:80/tcp` or
`198.51.100.1:5000/udp`. A real is written as its IPv4 address, `10.0.0.21`.

## Commands

| cmd | arguments | result |
|---|---|---|
| `ping` | | `"pong"` |
| `vip.add` | `vip`, optional `no_conntrack` (bool) | `{"vip_id": n}` |
| `vip.del` | `vip` | `{}` |
| `vip.list` | | `[{"vip", "vip_id", "flags", "generation", "reals": [{"addr","real_id","weight","up","draining","in_ring","mac"}]}]` `flags` is `PB_VIP_F_*` (1 = no conntrack). `generation` counts ring swaps since the daemon started. `in_ring` says whether the real currently owns slots (weight > 0, not draining, up, MAC resolved). `mac` is the destination MAC written to `neigh`, or `null` while unresolved |
| `real.add` | `vip`, `addr` (not `0.0.0.0`), optional `weight` (default 1, max 1000) | `{"real_id": n}` real_id is per address and shared by every VIP that uses the real. It is never 0 |
| `real.del` | `vip`, `addr` | `{}` |
| `real.weight` | `vip`, `addr`, `weight` | `{}` |
| `real.drain` | `vip`, `addr` | `{}` the real takes no ring slots, as if its weight were 0, so no new flow hashes to it. Existing flows keep going through the connection table. The configured weight is kept (and still reported). Health checks continue. |
| `real.undrain` | `vip`, `addr` | `{}` the real takes slots for its configured weight again (if it is up) |
| `stats` | optional `vip` | `{"global": {counter: n, ...}, "vips": {"<vip>": {counter: n, ...}}, "reals": {"<addr>": {"packets": n, "bytes": n}}}` counters are the names in `enum pb_counter` lower-cased without the `PB_CNT_` prefix, summed over CPUs |
| `flows` | optional `vip`, optional `real`, optional `limit` (default 1000) | `[{"src","dst","sport","dport","proto","real","vip","age_ms","cpu"}]` walks the LRU and returns one row per (key, CPU) that holds a value. The other CPUs' zero-filled copies are skipped. `real` is `"#<real_id>"` if the id is no longer allocated |
| `ring.show` | `vip` | `{"size": 65537, "hash": "maglev"\|"modulo", "slots": {"<addr>": n, "none": n}, "generation": n}` |
| `health` | | `[{"vip","addr","up","checked","consecutive_ok","consecutive_fail","last_change_ms","last_rtt_us"}]` `checked` is false for reals of UDP VIPs and when health checks are off; those reals are always `up`. `last_change_ms` is Unix epoch milliseconds |
| `reload` | | `{}` re-reads the YAML config, applies VIP and real differences without touching the connection table. Drops runtime-only changes. `interface`, `xdp_mode`, `conntrack.size`, `socket`, `pin_path`, `metrics.listen` and `health_check.enabled` take effect on restart only (the daemon logs a warning) |
| `config` | | the effective daemon config: `config_path`, `interface`, `xdp_mode` (in effect), `xdp_mode_requested`, `hash`, `conntrack` `{enabled, size}`, `encap_src_prefix`, `icmp_pmtu`, `next_hop`, `socket`, `pin_path`, `metrics_listen`, `health_check` `{enabled, interval_ms, timeout_ms, fall, rise}`, `detach_on_exit`, `num_possible_cpus`, `vips` (as configured in the YAML, not runtime changes) |

Errors are strings such as `"unknown vip 198.51.100.1:80/tcp"`, `"real already exists"`,
`"unknown real 10.0.0.25 on vip 198.51.100.1:80/tcp"`, `"too many vips (max 64)"`,
`"argument 'weight' must be a non-negative integer"`. `pbctl` prints `error: <message>` to
stderr and exits 1.

## pbctl

```
pbctl [--socket PATH] [--json] <command> ...

pbctl vip add 198.51.100.1:80/tcp [--no-conntrack]
pbctl vip del 198.51.100.1:80/tcp
pbctl vip list
pbctl real add 198.51.100.1:80/tcp 10.0.0.21 [--weight 2]
pbctl real del 198.51.100.1:80/tcp 10.0.0.21
pbctl real weight 198.51.100.1:80/tcp 10.0.0.21 4
pbctl real drain 198.51.100.1:80/tcp 10.0.0.21
pbctl real undrain 198.51.100.1:80/tcp 10.0.0.21
pbctl stats [--vip VIP]
pbctl flows [--vip VIP] [--real ADDR] [--limit N]
pbctl ring show 198.51.100.1:80/tcp
pbctl health
pbctl reload
pbctl config
```

Without `--json`, `pbctl` prints a human table. With `--json` it prints the raw
`result` object, which is what the lab scripts consume.

## Metrics

`GET /metrics` on `127.0.0.1:9101` (configurable), Prometheus text format, read from
the same per-CPU maps and summed.

```
pb_packets_total{vip="198.51.100.1:80/tcp"}
pb_bytes_total{vip=...}
pb_real_packets_total{real="10.0.0.21"}
pb_conntrack_hits_total{vip=...}
pb_conntrack_misses_total{vip=...}
pb_hash_total{vip=...}
pb_syn_total{vip=...}
pb_tx_total{vip=...}
pb_pass_total
pb_drops_total{vip=...,reason="frag"|"opts"|"no_real"|"adj_head"|"mtu"|"short"|"other"}
pb_real_up{vip=...,real=...} 0|1
pb_real_weight{vip=...,real=...}
pb_ring_generation{vip=...}
pb_conntrack_entries
pb_xdp_mode{mode="native"|"generic"} 1
```

Drops counted before the VIP is known (fragments, IP options, truncated headers, a
missing config) carry `vip="global"`. `pb_real_weight` is the configured weight, which
a drain does not change. `pb_conntrack_entries` is the number of keys (flows) in the
table, not per-CPU values, and costs an O(entries) walk per scrape.

`tx` (`pb_tx_total`, and `tx` in `pbctl stats`) counts `XDP_TX` verdicts: packets the
program encapsulated and handed back to the driver. It is not the number of frames the
driver transmitted. A driver whose transmit ring is full drops the frame after the
program returns, and the program cannot see that drop. On the lab's veth at saturation,
`tx` read 2.6 times the packets that reached the reals (README, Experiment 1). The
per-real `pb_real_packets_total` is counted at the same point and has the same meaning.
Monitor the driver's own counter beside it: `ethtool -S <dev>`, `xdp_tx_errors` on veth
(per queue, `rx_queue_N_xdp_tx_errors`), or the NIC driver's equivalent. See
[RUNBOOK.md](RUNBOOK.md).
