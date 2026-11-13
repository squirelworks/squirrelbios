#!/usr/bin/env python3
# A tiny routed network for the x86-netboot-routed smoke test.
# Original code.
#
# Usage: netsim.py LISTEN_PORT QEMU_PORT TFTP_DIR BOOT_FILE [norouter]
#
# QEMU joins it with
#   -netdev socket,id=n0,udp=127.0.0.1:LISTEN_PORT,localaddr=127.0.0.1:QEMU_PORT
# and every Ethernet frame travels as one UDP datagram on the loopback.
#
# The network: the guest's segment is 10.0.2.0/24 with a router at
# 10.0.2.1 (Ethernet 52:54:00:00:00:01), which is also the DHCP server.
# The boot server, 192.168.50.5, is on another network behind that
# router. DHCP hands out 10.0.2.15 with mask 255.255.255.0 (option 1),
# router 10.0.2.1 (option 3), siaddr 192.168.50.5 and the boot file
# (option 67). Only 10.0.2.1 answers ARP, and TFTP for 192.168.50.5 is
# served only when its frame is sent to the router's Ethernet address,
# so a client that does not route never gets the file. With norouter,
# DHCP leaves option 3 out. Exits when QEMU
# goes away (no frame for 30 s) or on SIGTERM.
import os
import socket
import struct
import sys

ROUTER_MAC = bytes.fromhex('525400000001')
ROUTER_IP = socket.inet_aton('10.0.2.1')
CLIENT_IP = socket.inet_aton('10.0.2.15')
MASK = socket.inet_aton('255.255.255.0')
SERVER_IP = socket.inet_aton('192.168.50.5')
BCAST_MAC = b'\xff' * 6
TFTP_PORT = 3069            # the server's transfer port (its TID)


def checksum(data):
    if len(data) % 2:
        data += b'\0'
    s = sum(struct.unpack('!%dH' % (len(data) // 2), data))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF


def udp_frame(dst_mac, src_ip, dst_ip, sport, dport, payload):
    udp = struct.pack('!HHHH', sport, dport, 8 + len(payload), 0) + payload
    ip = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(udp), 0, 0, 64, 17, 0,
                     src_ip, dst_ip)
    ip = ip[:10] + struct.pack('!H', checksum(ip)) + ip[12:]
    return dst_mac + ROUTER_MAC + b'\x08\x00' + ip + udp


def dhcp_reply(request, kind, boot_file, router=True):
    xid = request[4:8]
    chaddr = request[28:44]
    bootp = bytearray(236)
    bootp[0:4] = b'\x02\x01\x06\x00'
    bootp[4:8] = xid
    bootp[10:12] = b'\x80\x00'
    bootp[16:20] = CLIENT_IP
    bootp[20:24] = SERVER_IP
    bootp[28:44] = chaddr
    opts = b'\x63\x82\x53\x63'
    opts += bytes([53, 1, kind])
    opts += bytes([54, 4]) + ROUTER_IP
    opts += bytes([51, 4]) + struct.pack('!I', 3600)
    opts += bytes([1, 4]) + MASK
    if router:
        opts += bytes([3, 4]) + ROUTER_IP
    name = boot_file.encode()
    opts += bytes([67, len(name)]) + name
    opts += b'\xff'
    payload = bytes(bootp) + opts
    payload += b'\0' * max(0, 300 - len(payload))
    return udp_frame(BCAST_MAC, ROUTER_IP, b'\xff' * 4, 67, 68, payload)


def main():
    listen, qemu_port, tftp_dir, boot_file = sys.argv[1:5]
    router = sys.argv[5:6] != ['norouter']
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(('127.0.0.1', int(listen)))
    sock.settimeout(30)
    peer = ('127.0.0.1', int(qemu_port))
    data = b''
    client_mac = None
    client_port = None
    while True:
        try:
            frame, _ = sock.recvfrom(4096)
        except socket.timeout:
            return
        if len(frame) < 42:
            continue
        dst, src, etype = frame[0:6], frame[6:12], frame[12:14]
        if etype == b'\x08\x06':                    # ARP
            op = frame[20:22]
            target = frame[38:42]
            if op == b'\x00\x01' and target == ROUTER_IP:
                reply = (src + ROUTER_MAC + b'\x08\x06' +
                         b'\x00\x01\x08\x00\x06\x04\x00\x02' +
                         ROUTER_MAC + ROUTER_IP + src + frame[28:32])
                sock.sendto(reply, peer)
            continue
        if etype != b'\x08\x00' or frame[23] != 17:
            continue
        ihl = (frame[14] & 0x0F) * 4
        dst_ip = frame[30:34]
        udp = frame[14 + ihl:]
        sport, dport, ulen = struct.unpack('!HHH', udp[0:6])
        payload = udp[8:ulen]
        if dport == 67 and len(payload) >= 240:
            kind = 0
            i = 240
            while i < len(payload) and payload[i] != 0xFF:
                if payload[i] == 0:
                    i += 1
                    continue
                if payload[i] == 53:
                    kind = payload[i + 2]
                i += 2 + payload[i + 1]
            client_mac = src
            if kind == 1:
                sock.sendto(dhcp_reply(payload, 2, boot_file, router), peer)
            elif kind == 3:
                sock.sendto(dhcp_reply(payload, 5, boot_file, router), peer)
            continue
        # Everything for the boot server has to come through the router
        if dst_ip != SERVER_IP or dst != ROUTER_MAC:
            continue
        op = struct.unpack('!H', payload[0:2])[0]
        if dport == 69 and op == 1:                 # read request
            name = payload[2:].split(b'\0')[0].decode()
            path = os.path.join(tftp_dir, os.path.basename(name))
            client_port = sport
            try:
                data = open(path, 'rb').read()
            except OSError:
                err = struct.pack('!HH', 5, 1) + b'not found\0'
                sock.sendto(udp_frame(src, SERVER_IP, CLIENT_IP, TFTP_PORT,
                                      sport, err), peer)
                continue
            block = 1
        elif dport == TFTP_PORT and op == 4:        # acknowledgement
            block = struct.unpack('!H', payload[2:4])[0] + 1
            if (block - 1) * 512 > len(data):
                continue                            # the last one
        else:
            continue
        chunk = data[(block - 1) * 512:block * 512]
        msg = struct.pack('!HH', 3, block & 0xFFFF) + chunk
        sock.sendto(udp_frame(client_mac or src, SERVER_IP, CLIENT_IP,
                              TFTP_PORT, client_port, msg), peer)


if __name__ == '__main__':
    main()
