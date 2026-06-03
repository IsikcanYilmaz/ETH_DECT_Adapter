#!/usr/bin/env python3

from scapy.all import * 
import sys
import binascii
import argparse

"""
Forge a custom IP Packet and send it to our Pico interface
"""

def main(iface=1, length=0):
    sourceMAC="00:11:22:33:44:55:66"
    destinationMAC="CA:FE:00:00:00:01"
    ifacename="enxcafe00000001"

    if (iface == 2):
        ifacename="enxcafe00000002"
        destinationMAC="CA:FE:00:00:00:02"

    destinationIP="111.222.111.2"

    etherTypeHex = "0x0800"
    etherType = int(etherTypeHex, 16)

    plStr = "IMAMHATIPLERKAPATILSIN"
    payloadLength = len(plStr) if length == 0 else length 

    payload = bytearray(plStr, 'utf-8')

    count = 0
    while (payloadLength > len(payload)):
        # payload += '!'
        payload.append(count % 0xff) 
        count += 1

    packet = Ether(src=sourceMAC, dst=destinationMAC,type=etherType) / IP(dst=destinationIP) / payload

    print("Hexdump:")
    chexdump(packet)
    print(f"\nPacket length {len(packet)} + 6 byte header {len(packet) + 6}")
    srp(packet, iface=ifacename, timeout=0)

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--two", action="store_true", default=False)
    parser.add_argument("--len", type=int, default=0)
    args = parser.parse_args()
    main(iface=2 if args.two else 1, length=args.len)
