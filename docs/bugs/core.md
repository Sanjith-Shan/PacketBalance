# Core bugs

Real defects found while building and testing the core (Maglev, flow hash,
VIP parsing). Each entry: what, how it was found, status.

## VipSpec::parse accepts trailing junk and signs in the port

`include/packetbalance/vipspec.h` parses the port with `std::stoi`, which stops
at the first non-digit and skips leading whitespace and a sign. So
`198.51.100.1:80x/tcp`, `198.51.100.1: 80/tcp` and `198.51.100.1:+80/tcp` all
parse as `198.51.100.1:80/tcp`, and `parse_ipv4` (sscanf `%u`) accepts
`1.2.3.-0`. A typo in a config file or a `pbctl` argument is silently accepted
instead of rejected.

Found by probing the parser while writing `tests/core/vipspec_test.cpp`.
Status: fixed. vipspec.h is now strict: four decimal octets of digits only, and
a port of ASCII digits only in 1..65535.

## hash.h comment says it is the kernel's jhash_3words; it is not

The header says the flow hash is "Bob Jenkins' lookup3 jhash as the Linux
kernel uses it". The kernel's `jhash_3words(a, b, c, initval)` adds
`initval + JHASH_INITVAL + (3 << 2)` to all three words before the final mix;
`pb_jhash_3words` adds `JHASH_INITVAL` to a and b and `initval` to c. Same
final mix, different pre-mix, so the values differ from the kernel's.

Harmless for correctness, because the XDP program and the control plane both
include hash.h and never call the kernel's jhash, and
`tests/core/hash_test.cpp` pins the current values. But anyone who swaps in the
kernel function "because the comment says it is the same" would break lb1/lb2
agreement. Status: fixed. The comment now says the function is not
bit-identical to the kernel's (the function is unchanged, since changing it
would re-shuffle every ring).
