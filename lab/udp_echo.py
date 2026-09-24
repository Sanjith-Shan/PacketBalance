#!/usr/bin/env python3
"""Tiny UDP echo server for the lab reals: udp_echo.py PORT.

One socket bound to 0.0.0.0, so it answers datagrams addressed to the VIP
(on lo) as well as to the real's own address. Single threaded on purpose:
it exists for functional checks, not for load. lab/measure.sh pauses it
(SIGSTOP) while pktgen floods port 5000.
"""
import socket
import sys

port = int(sys.argv[1]) if len(sys.argv) > 1 else 5000
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", port))
while True:
    data, addr = s.recvfrom(65535)
    try:
        s.sendto(data, addr)
    except OSError:
        pass
