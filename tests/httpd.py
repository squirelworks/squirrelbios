#!/usr/bin/env python3
# A small HTTP/1.1 server for the UEFI HTTP boot smoke tests.
# Original code.
#
# Usage: httpd.py PORT DIR
#
# Serves the files in DIR on 127.0.0.1:PORT (QEMU's user network shows it
# to the guest as 10.0.2.2:PORT):
#   /NAME            the file, with Content-Length
#   /chunked/NAME    the file in chunked transfer coding (odd chunk sizes)
#   /close/NAME      the file with neither, ended by closing the connection
#   /redirect/NAME   302 to /chunked/NAME
# Anything else is a 404. Exits on SIGTERM.
import os
import socketserver
import sys
from http.server import BaseHTTPRequestHandler


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def do_GET(self):
        parts = self.path.lstrip('/').split('/')
        mode = parts[0] if len(parts) == 2 else ''
        name = parts[-1]
        path = os.path.join(sys.argv[2], name)
        if '..' in name or not os.path.isfile(path) or mode not in ('', 'chunked', 'close', 'redirect'):
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
    Server(('127.0.0.1', int(sys.argv[1])), Handler).serve_forever()
