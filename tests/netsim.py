#!/usr/bin/env python3
# A tiny routed network for the UEFI netboot / HTTP-boot-with-DNS smoke
# tests. Original code.
#
# Usage: netsim.py LISTEN_PORT QEMU_PORT TFTP_DIR BOOT_FILE [norouter]
#
# QEMU joins it with
#   -netdev socket,id=n0,udp=127.0.0.1:LISTEN_PORT,localaddr=127.0.0.1:QEMU_PORT
# and every Ethernet frame travels as one UDP datagram on the loopback.
#
# The network: the guest's segment is 10.0.2.0/24 with a router at
# 10.0.2.1 (Ethernet 52:54:00:00:00:01), which is also the DHCP server
# and the DNS server (option 6). The boot server, 192.168.50.5, is on
# another network behind that router for TFTP. DHCP hands out 10.0.2.15
# with mask 255.255.255.0 (option 1), router 10.0.2.1 (option 3), DNS
# 10.0.2.1 (option 6), siaddr 192.168.50.5 (or 10.0.2.1 when the boot
# file is an http:// URL) and the boot file (option 67).
#
# DNS (UDP/53): A records for boot.squirrel.test → 10.0.2.1; anything
# else is NXDOMAIN. When the boot file is http://..., the router also
# answers TCP/80 with a one-shot HTTP/1.1 response of the named file
# from TFTP_DIR (Content-Length, Connection: close).
#
# Only 10.0.2.1 answers ARP, and TFTP for 192.168.50.5 is served only
# when its frame is sent to the router's Ethernet address, so a client
# that does not route never gets the file. With norouter, DHCP leaves
# option 3 out. Exits when QEMU goes away (no frame for NETSIM_IDLE
# seconds, default 30) or on SIGTERM.
#
# Several QEMU nodes may join (diskless boot, roadmap F4), each with
# localaddr on a port of its own and a MAC address of its own: every
# frame is answered to the node that sent it, and the node whose MAC ends
# in 56 (QEMU's default) gets 10.0.2.15, the one ending in 57 10.0.2.16,
# and so on. The TFTP state is kept per node.
import os
import socket
import struct
import sys

ROUTER_MAC = bytes.fromhex('525400000001')
ROUTER_IP = socket.inet_aton('10.0.2.1')
# QEMU user-network host address: the SNP test app ARPs for it after HTTP
# boot, so answer as a neighbour on this segment too.
HOST_MAC = bytes.fromhex('525400000002')
HOST_IP = socket.inet_aton('10.0.2.2')
MASK = socket.inet_aton('255.255.255.0')
SERVER_IP = socket.inet_aton('192.168.50.5')
BCAST_MAC = b'\xff' * 6
TFTP_PORT = 3069            # the server's transfer port (its TID)
DNS_NAME = 'boot.squirrel.test'
HTTP_PORT = 80
TCP_MSS = 1400


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


def tcp_frame(dst_mac, src_ip, dst_ip, sport, dport, seq, ack, flags, payload=b'',
              window=65535):
    doff = 5
    tcp = struct.pack('!HHIIBBHHH', sport, dport, seq & 0xFFFFFFFF, ack & 0xFFFFFFFF,
                      (doff << 4), flags, window, 0, 0) + payload
    pseudo = src_ip + dst_ip + struct.pack('!BBH', 0, 6, len(tcp))
    tcp = tcp[:16] + struct.pack('!H', checksum(pseudo + tcp)) + tcp[18:]
    ip = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(tcp), 0, 0x4000, 64, 6, 0,
                     src_ip, dst_ip)
    ip = ip[:10] + struct.pack('!H', checksum(ip)) + ip[12:]
    return dst_mac + ROUTER_MAC + b'\x08\x00' + ip + tcp


def client_ip(mac):
    return bytes([10, 0, 2, (15 + mac[5] - 0x56) & 0xFF])


def dhcp_reply(request, kind, boot_file, router=True, http=False):
    xid = request[4:8]
    chaddr = request[28:44]
    bootp = bytearray(236)
    bootp[0:4] = b'\x02\x01\x06\x00'
    bootp[4:8] = xid
    bootp[10:12] = b'\x80\x00'
    bootp[16:20] = client_ip(chaddr)
    bootp[20:24] = ROUTER_IP if http else SERVER_IP
    bootp[28:44] = chaddr
    opts = b'\x63\x82\x53\x63'
    opts += bytes([53, 1, kind])
    opts += bytes([54, 4]) + ROUTER_IP
    opts += bytes([51, 4]) + struct.pack('!I', 3600)
    opts += bytes([1, 4]) + MASK
    if router:
        opts += bytes([3, 4]) + ROUTER_IP
    opts += bytes([6, 4]) + ROUTER_IP          # DNS (option 6)
    name = boot_file.encode()
    opts += bytes([67, len(name)]) + name
    opts += b'\xff'
    payload = bytes(bootp) + opts
    payload += b'\0' * max(0, 300 - len(payload))
    return udp_frame(BCAST_MAC, ROUTER_IP, b'\xff' * 4, 67, 68, payload)


def dns_labels(name):
    out = b''
    for lab in name.split('.'):
        out += bytes([len(lab)]) + lab.encode()
    return out + b'\0'


def dns_reply(query, qname):
    """Answer an A query for boot.squirrel.test; NXDOMAIN otherwise."""
    if len(query) < 12:
        return None
    tid = query[0:2]
    flags = 0x8180                                  # response, RD, RA
    # skip header and question name
    i = 12
    name = []
    while i < len(query):
        n = query[i]
        if n == 0:
            i += 1
            break
        if n & 0xC0:
            return None
        name.append(query[i + 1:i + 1 + n].decode('ascii', 'replace'))
        i += 1 + n
    else:
        return None
    if i + 4 > len(query):
        return None
    qtype, qclass = struct.unpack('!HH', query[i:i + 4])
    asked = '.'.join(name).lower()
    if asked == qname.lower() and qtype == 1 and qclass == 1:
        # pointer to the question name at offset 12
        answer = struct.pack('!HHHLH4s', 0xC00C, 1, 1, 60, 4, ROUTER_IP)
        return tid + struct.pack('!HHHHH', flags, 1, 1, 0, 0) + query[12:i + 4] + answer
    flags = 0x8183                                  # NXDOMAIN
    return tid + struct.pack('!HHHHH', flags, 1, 0, 0, 0) + query[12:i + 4]


class HttpConn:
    """One stop-and-wait HTTP/1.1 response over TCP for the firmware client."""

    def __init__(self, peer_mac, peer_ip, sport, dport, seq, file_data):
        self.mac = peer_mac
        self.ip = peer_ip
        self.sport = dport          # our port (80)
        self.dport = sport          # client's
        self.rcv_nxt = (seq + 1) & 0xFFFFFFFF
        self.iss = 1000
        self.snd_nxt = self.iss
        self.snd_una = self.iss
        self.state = 'syn'
        self.req = b''
        self.body = file_data
        self.tx = b''
        self.tx_off = 0
        self.fin_sent = False

    def syn_ack(self):
        self.snd_nxt = (self.iss + 1) & 0xFFFFFFFF
        self.snd_una = self.iss
        return tcp_frame(self.mac, ROUTER_IP, self.ip, self.sport, self.dport,
                         self.iss, self.rcv_nxt, 0x12)   # SYN+ACK

    def on_segment(self, seq, ack, flags, payload):
        out = []
        if flags & 0x04:                            # RST
            self.state = 'closed'
            return out
        if self.state == 'syn':
            if (flags & 0x10) and ack == self.snd_nxt:
                self.snd_una = ack
                self.state = 'est'
            else:
                return out
        if payload:
            if seq == self.rcv_nxt:
                self.req += payload
                self.rcv_nxt = (self.rcv_nxt + len(payload)) & 0xFFFFFFFF
                out.append(tcp_frame(self.mac, ROUTER_IP, self.ip, self.sport,
                                     self.dport, self.snd_nxt, self.rcv_nxt, 0x10))
                if b'\r\n\r\n' in self.req and not self.tx:
                    path = b'/app.efi'
                    line = self.req.split(b'\r\n', 1)[0]
                    parts = line.split(b' ')
                    if len(parts) >= 2 and parts[1].startswith(b'/'):
                        path = parts[1].split(b'?', 1)[0]
                    name = os.path.basename(path.decode('ascii', 'replace'))
                    data = self.body.get(name)
                    if data is None:
                        msg = b'HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n'
                    else:
                        msg = (b'HTTP/1.1 200 OK\r\nContent-Type: application/efi\r\n'
                               b'Content-Length: %d\r\nConnection: close\r\n\r\n' % len(data) + data)
                    self.tx = msg
            else:
                out.append(tcp_frame(self.mac, ROUTER_IP, self.ip, self.sport,
                                     self.dport, self.snd_nxt, self.rcv_nxt, 0x10))
        if flags & 0x10 and ack != 0:
            self.snd_una = ack
        if self.state == 'est' and self.tx and self.tx_off < len(self.tx) and self.snd_una == self.snd_nxt:
            chunk = self.tx[self.tx_off:self.tx_off + TCP_MSS]
            if chunk:
                out.append(tcp_frame(self.mac, ROUTER_IP, self.ip, self.sport,
                                     self.dport, self.snd_nxt, self.rcv_nxt,
                                     0x18, chunk))  # PSH+ACK
                self.snd_nxt = (self.snd_nxt + len(chunk)) & 0xFFFFFFFF
                self.tx_off += len(chunk)
        if (self.state == 'est' and self.tx and self.tx_off >= len(self.tx) and
                not self.fin_sent and self.snd_una == self.snd_nxt):
            out.append(tcp_frame(self.mac, ROUTER_IP, self.ip, self.sport,
                                 self.dport, self.snd_nxt, self.rcv_nxt, 0x11))  # FIN+ACK
            self.snd_nxt = (self.snd_nxt + 1) & 0xFFFFFFFF
            self.fin_sent = True
            self.state = 'fin'
        if flags & 0x01 and self.state in ('est', 'fin'):
            self.rcv_nxt = (seq + 1) & 0xFFFFFFFF
            out.append(tcp_frame(self.mac, ROUTER_IP, self.ip, self.sport,
                                 self.dport, self.snd_nxt, self.rcv_nxt, 0x10))
            self.state = 'closed'
        return out


def load_http_files(tftp_dir):
    files = {}
    if not os.path.isdir(tftp_dir):
        return files
    for name in os.listdir(tftp_dir):
        path = os.path.join(tftp_dir, name)
        if os.path.isfile(path):
            with open(path, 'rb') as f:
                files[name] = f.read()
    return files


def main():
    listen, qemu_port, tftp_dir, boot_file = sys.argv[1:5]
    router = sys.argv[5:6] != ['norouter']
    http = boot_file.startswith('http://')
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(('127.0.0.1', int(listen)))
    # A large node (f4-scale, hundreds of CPUs) can take a minute before
    # its first DHCP; NETSIM_IDLE keeps the server waiting for it.
    sock.settimeout(int(os.environ.get('NETSIM_IDLE', '30')))
    del qemu_port               # each node is answered where it sent from
    nodes = {}                  # MAC: [file data, client port]
    tcp_conns = {}              # (mac, sport): HttpConn
    http_files = load_http_files(tftp_dir) if http else {}
    while True:
        try:
            frame, peer = sock.recvfrom(4096)
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
            elif op == b'\x00\x01' and target == HOST_IP:
                reply = (src + HOST_MAC + b'\x08\x06' +
                         b'\x00\x01\x08\x00\x06\x04\x00\x02' +
                         HOST_MAC + HOST_IP + src + frame[28:32])
                sock.sendto(reply, peer)
            continue
        if etype != b'\x08\x00':
            continue
        ihl = (frame[14] & 0x0F) * 4
        tot = struct.unpack('!H', frame[16:18])[0]
        if tot < ihl or 14 + tot > len(frame):
            continue
        proto = frame[23]
        dst_ip = frame[30:34]
        ip_payload = frame[14 + ihl:14 + tot]
        if proto == 17:                             # UDP
            udp = ip_payload
            if len(udp) < 8:
                continue
            sport, dport, ulen = struct.unpack('!HHH', udp[0:6])
            if ulen < 8 or ulen > len(udp):
                continue
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
                if kind == 1:
                    sock.sendto(dhcp_reply(payload, 2, boot_file, router, http), peer)
                elif kind == 3:
                    sock.sendto(dhcp_reply(payload, 5, boot_file, router, http), peer)
                continue
            if dport == 53 and dst_ip == ROUTER_IP:
                reply = dns_reply(payload, DNS_NAME)
                if reply:
                    sock.sendto(udp_frame(src, ROUTER_IP, client_ip(src), 53, sport, reply),
                                peer)
                continue
            # Everything for the boot server has to come through the router
            if dst_ip != SERVER_IP or dst != ROUTER_MAC:
                continue
            op = struct.unpack('!H', payload[0:2])[0]
            node = nodes.setdefault(src, [b'', None])
            if dport == 69 and op == 1:                 # read request
                name = payload[2:].split(b'\0')[0].decode()
                path = os.path.join(tftp_dir, os.path.basename(name))
                node[1] = sport
                try:
                    node[0] = open(path, 'rb').read()
                except OSError:
                    err = struct.pack('!HH', 5, 1) + b'not found\0'
                    sock.sendto(udp_frame(src, SERVER_IP, client_ip(src), TFTP_PORT,
                                          sport, err), peer)
                    continue
                block = 1
            elif dport == TFTP_PORT and op == 4:        # acknowledgement
                block = struct.unpack('!H', payload[2:4])[0] + 1
                if (block - 1) * 512 > len(node[0]):
                    continue                            # the last one
            else:
                continue
            chunk = node[0][(block - 1) * 512:block * 512]
            msg = struct.pack('!HH', 3, block & 0xFFFF) + chunk
            sock.sendto(udp_frame(src, SERVER_IP, client_ip(src),
                                  TFTP_PORT, node[1], msg), peer)
            continue
        if proto == 6 and http and dst_ip == ROUTER_IP:  # TCP HTTP
            tcp = ip_payload
            if len(tcp) < 20:
                continue
            sport, dport, seq, ack, off_flags = struct.unpack('!HHIIH', tcp[0:14])
            flags = off_flags & 0x01FF
            doff = (off_flags >> 12) * 4
            if doff < 20 or doff > len(tcp):
                continue
            payload = tcp[doff:]
            if dport != HTTP_PORT:
                continue
            key = (src, sport)
            conn = tcp_conns.get(key)
            if flags & 0x02 and not (flags & 0x10):     # SYN
                conn = HttpConn(src, client_ip(src), sport, dport, seq, http_files)
                tcp_conns[key] = conn
                sock.sendto(conn.syn_ack(), peer)
                continue
            if not conn:
                continue
            for pkt in conn.on_segment(seq, ack, flags, payload):
                sock.sendto(pkt, peer)
            if conn.state == 'closed':
                tcp_conns.pop(key, None)


if __name__ == '__main__':
    main()
