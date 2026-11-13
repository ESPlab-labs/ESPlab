#!/usr/bin/env python3
"""listen on the vboard multicast group and print what arrives (debug tool)"""
import socket, struct, sys

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", 7777))
mreq = struct.pack("4sl", socket.inet_aton("239.255.77.88"), socket.INADDR_ANY)
s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
s.settimeout(3.0)
n = 0
try:
    while n < 8:
        data, addr = s.recvfrom(300)
        n += 1
        print("got", len(data), "bytes from", addr[0], "head:", data[:12].hex())
except socket.timeout:
    if n == 0:
        print("NOTHING received on group in 3s")
    else:
        print("total packets:", n)
