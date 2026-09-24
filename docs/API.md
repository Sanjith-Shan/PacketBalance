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
| `vip.list` | | `[{"vip", "vip_id", "flags", "reals": [{"addr","real_id","weight","up","draining"}]}]` |
| `real.add` | `vip`, `addr`, optional `weight` (default 1) | `{"real_id": n}` |
| `real.del` | `vip`, `addr` | `{}` |
| `real.weight` | `vip`, `addr`, `weight` | `{}` |
| `real.drain` | `vip`, `addr` | `{}` weight becomes 0 so no new flow hashes to it, existing flows keep going through the connection table. Health checks continue. |
| `real.undrain` | `vip`, `addr` | `{}` restores the configured weight |
| `stats` | optional `vip` | `{"global": {counter: n, ...}, "vips": {"<vip>": {counter: n, ...}}, "reals": {"<addr>": {"packets": n, "bytes": n}}}` counters are the names in `enum pb_counter` lower-cased without the `PB_CNT_` prefix, summed over CPUs |
| `flows` | optional `vip`, optional `real`, optional `limit` (default 1000) | `[{"src","dst","sport","dport","proto","real","vip","age_ms","cpu"}]` walks every per-CPU LRU entry |
| `ring.show` | `vip` | `{"size": 65537, "hash": "maglev"|"modulo", "slots": {"<addr>": n, "none": n}, "generation": n}` |
| `health` | | `[{"vip","addr","up","consecutive_ok","consecutive_fail","last_change_ms","last_rtt_us"}]` |
| `reload` | | `{}` re-reads the YAML config, applies VIP and real differences without touching the connection table |
| `config` | | the effective daemon config as JSON (interface, xdp_mode, hash, conntrack, encap source, health check parameters) |

Errors are strings such as `"unknown vip 198.51.100.1:80/tcp"`, `"real already exists"`,
`"ring full"`. `pbctl` prints `error: <message>` to stderr and exits 1.

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
```
