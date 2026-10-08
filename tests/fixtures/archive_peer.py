#!/usr/bin/env python3
"""Serve source bytes and count requests for acquisition checks."""
import http.server
from pathlib import Path
import sys
root=Path(sys.argv[1])
class Peer(http.server.BaseHTTPRequestHandler):
    def log_message(self,*_args): pass
    def do_GET(self):
        with (root/'requests').open('a') as output: output.write(self.path+'\n')
        payload=(root/'payload.tar.gz').read_bytes()
        self.send_response(200); self.send_header('Content-Length',str(len(payload)))
        self.end_headers(); self.wfile.write(payload)
server=http.server.HTTPServer(('127.0.0.1',0),Peer)
(root/'port').write_text(str(server.server_port))
server.serve_forever(poll_interval=.05)
