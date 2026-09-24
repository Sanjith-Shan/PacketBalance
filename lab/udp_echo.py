#!/usr/bin/env python3
"""Tiny UDP echo server for the lab reals: udp_echo.py PORT.

One socket bound to 0.0.0.0. Replies are sent FROM the address the datagram
was sent TO (IP_PKTINFO), so a datagram for the VIP is answered from the VIP.
Without that, an unconnected UDP socket picks the source by routing (the
real's 10.0.0.2x address) and the client, which sent to the VIP, discards the
reply: the DSR version of "UDP works in tcpdump but not in the app".
Single threaded on purpose, it exists for functional checks, not for load.
lab/measure.sh pauses it (SIGSTOP) while pktgen floods port 5000.
"""
import socket
import struct
import sys

IP_PKTINFO = getattr(socket, "IP_PKTINFO", 8)
port = int(sys.argv[1]) if len(sys.argv) > 1 else 5000
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.setsockopt(socket.IPPROTO_IP, IP_PKTINFO, 1)
s.bind(("0.0.0.0", port))
while True:
    data, anc, _flags, addr = s.recvmsg(65535, socket.CMSG_SPACE(12))
    cmsgs = []
    for level, ctype, cdata in anc:
        if level == socket.IPPROTO_IP and ctype == IP_PKTINFO and len(cdata) >= 12:
            _ifindex, _spec, dst = struct.unpack("i4s4s", cdata[:12])
            # ifindex 0: let routing pick the interface, spec_dst = reply source
            cmsgs.append((socket.IPPROTO_IP, IP_PKTINFO, struct.pack("i4s4s", 0, dst, b"\0" * 4)))
    try:
        s.sendmsg([data], cmsgs, 0, addr)
    except OSError:
        pass
