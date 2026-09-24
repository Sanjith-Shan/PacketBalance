// SPDX-License-Identifier: GPL-2.0
// A do-nothing XDP program for the ROOT-SIDE peers of the LB veths (pb-lb1,
// pb-lb2). A veth only accepts frames XDP_TX'd by its peer when its own NAPI
// runs, and attaching any XDP program is the reliable way to switch that on.
// Built by lab/up.sh with clang; see docs/bugs/lab.md.
#include <linux/bpf.h>
#define SEC(n) __attribute__((section(n), used))
SEC("xdp") int xdp_pass(struct xdp_md *ctx) { (void)ctx; return XDP_PASS; }
char _license[] SEC("license") = "GPL";
