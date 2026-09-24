/* SPDX-License-Identifier: MIT
 *
 * Flow hash shared by the XDP program and the control plane.
 *
 * Both sides MUST use this exact function. Load balancer failover
 * (Experiment 4) works only because lb1 and lb2 hash a 5-tuple to the same
 * ring slot, and the control plane's hash-quality experiment (Experiment 5)
 * is only meaningful if it uses the data plane's hash.
 *
 * This is the final mixing step of Bob Jenkins' lookup3 hash, the same mix the
 * Linux kernel's jhash_3words uses (include/linux/jhash.h). It is NOT
 * bit-identical to the kernel's jhash_3words, which also adds
 * (initval + JHASH_INITVAL + 12) before mixing. That does not matter: the only
 * requirement is that both planes use this one function. Inputs are taken in
 * network byte order exactly as stored in struct pb_ct_key.
 */
#ifndef PACKETBALANCE_HASH_H
#define PACKETBALANCE_HASH_H

#include "packetbalance/abi.h"

#define PB_JHASH_INITVAL 0xdeadbeefu
#define PB_HASH_SEED     0x50424C42u   /* "PBLB" */

static inline __u32 pb_rol32(__u32 word, unsigned int shift)
{
    return (word << (shift & 31)) | (word >> ((-shift) & 31));
}

#define PB_JHASH_FINAL(a, b, c)            \
    {                                      \
        c ^= b; c -= pb_rol32(b, 14);      \
        a ^= c; a -= pb_rol32(c, 11);      \
        b ^= a; b -= pb_rol32(a, 25);      \
        c ^= b; c -= pb_rol32(b, 16);      \
        a ^= c; a -= pb_rol32(c, 4);       \
        b ^= a; b -= pb_rol32(a, 14);      \
        c ^= b; c -= pb_rol32(b, 24);      \
    }

static inline __u32 pb_jhash_3words(__u32 a, __u32 b, __u32 c, __u32 initval)
{
    a += PB_JHASH_INITVAL;
    b += PB_JHASH_INITVAL;
    c += initval;
    PB_JHASH_FINAL(a, b, c);
    return c;
}

/* The 5-tuple flow hash. All fields in network byte order. */
static inline __u32 pb_flow_hash(__u32 saddr, __u32 daddr,
                                 __u16 sport, __u16 dport, __u8 proto)
{
    __u32 ports = ((__u32)sport << 16) | (__u32)dport;
    return pb_jhash_3words(saddr, daddr, ports, PB_HASH_SEED ^ (__u32)proto);
}

/* Hash used by Maglev to derive a backend's permutation (offset, skip).
 * Keyed by the real's IPv4 address, NOT by real_id, so two load balancers
 * with the same set of reals build the same ring even if their real_id
 * numbering differs. Two independent hashes, as the paper requires. */
static inline __u32 pb_backend_hash_offset(__u32 addr)
{
    return pb_jhash_3words(addr, 0x9e3779b9u, 0, 0x0f0f0f0fu);
}
static inline __u32 pb_backend_hash_skip(__u32 addr)
{
    return pb_jhash_3words(addr, 0x7f4a7c15u, 1, 0xf0f0f0f0u);
}

#endif /* PACKETBALANCE_HASH_H */
