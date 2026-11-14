#!/usr/bin/env python3
# A small HTTP/1.1 server for the UEFI HTTP boot smoke tests, with an
# optional tiny DNS A-record responder for name-based HTTP boot.
# Original code.
#
# Usage: httpd.py PORT DIR [DNS_PORT NAME=A.B.C.D ...]
#
# Serves the files in DIR on 127.0.0.1:PORT (QEMU's user network shows it
# to the guest as 10.0.2.2:PORT):
#   /NAME            the file, with Content-Length
#   /chunked/NAME    the file in chunked transfer coding (odd chunk sizes)
#   /close/NAME      the file with neither, ended by closing the connection
#   /redirect/NAME   302 to /chunked/NAME
#   /huge/NAME       a Content-Length of 3000000000, the file, then a close
# Anything else is a 404.
#
# When DNS_PORT and NAME=A.B.C.D arguments follow, a DNS responder on
# 127.0.0.1:DNS_PORT answers A queries for those names (and NXDOMAIN for
# anything else). The simulated network in netsim.py uses the same reply
# shape for boot.squirrel.test. Exits on SIGTERM.
import os
import socket
import socketserver
import struct
import sys
import threading
from http.server import BaseHTTPRequestHandler


def dns_reply(query, zones):
    """RFC 1035 A-record reply for names in zones {name: ipv4_bytes}; else NXDOMAIN."""
    if len(query) < 12:
        return None
    tid = query[0:2]
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
    addr = zones.get(asked)
    if addr and qtype == 1 and qclass == 1:
        answer = struct.pack('!HHHLH4s', 0xC00C, 1, 1, 60, 4, addr)
        return tid + struct.pack('!HHHHH', 0x8180, 1, 1, 0, 0) + query[12:i + 4] + answer
    return tid + struct.pack('!HHHHH', 0x8183, 1, 0, 0, 0) + query[12:i + 4]


def dns_serve(port, zones):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('127.0.0.1', port))
    while True:
        try:
            data, peer = sock.recvfrom(512)
        except OSError:
            return
        reply = dns_reply(data, zones)
        if reply:
            sock.sendto(reply, peer)


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def do_GET(self):
        parts = self.path.lstrip('/').split('/')
        mode = parts[0] if len(parts) == 2 else ''
        name = parts[-1]
        path = os.path.join(sys.argv[2], name)
        if '..' in name or not os.path.isfile(path) or mode not in ('', 'chunked', 'close', 'redirect', 'huge'):
            body = b'not here\n'
            self.send_response(404)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if mode == 'redirect':
            self.send_response(302)
            self.send_header('Location', '/chunked/' + name)
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        with open(path, 'rb') as f:
            data = f.read()
        self.send_response(200)
        self.send_header('Content-Type', 'application/efi')
        if mode == 'chunked':
            self.send_header('Transfer-Encoding', 'chunked')
            self.end_headers()
            i, size = 0, 1000
            while i < len(data):
                piece = data[i:i + size]
                self.wfile.write(b'%x;ext=1\r\n' % len(piece) + piece + b'\r\n')
                i += len(piece)
                size = size * 3 // 2 + 7
            self.wfile.write(b'0\r\nX-Trailer: yes\r\n\r\n')
        elif mode == 'huge':
            self.send_header('Content-Length', '3000000000')
            self.send_header('Connection', 'close')
            self.end_headers()
            self.wfile.write(data)
            self.close_connection = True
        elif mode == 'close':
            self.send_header('Connection', 'close')
            self.end_headers()
            self.wfile.write(data)
            self.close_connection = True
        else:
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)


class Server(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == '__main__':
    if len(sys.argv) > 3:
        dns_port = int(sys.argv[3])
        zones = {}
        for spec in sys.argv[4:]:
            name, addr = spec.split('=', 1)
            zones[name.lower()] = socket.inet_aton(addr)
        threading.Thread(target=dns_serve, args=(dns_port, zones), daemon=True).start()
    Server(('127.0.0.1', int(sys.argv[1])), Handler).serve_forever()
