#!/usr/bin/env python3
"""Remote lockd queue and mutual TLS wire data for CLI tests."""
from email.parser import BytesParser
from email.policy import default
import http.server
import json
from pathlib import Path
import ssl
import sys

root = Path(sys.argv[1])
class Peer(http.server.BaseHTTPRequestHandler):
    # Answer Expect: 100-continue before reading a streamed request body.
    # HTTP/1.0 made libcurl wait its one-second fallback on every POST.
    protocol_version = 'HTTP/1.1'
    def log_message(self, *_args):
        pass
    def do_POST(self):
        if self.headers.get('Transfer-Encoding') == 'chunked':
            data = bytearray()
            while True:
                size = int(self.rfile.readline().strip(), 16)
                if not size:
                    self.rfile.readline()
                    break
                data.extend(self.rfile.read(size))
                self.rfile.read(2)
        else:
            data = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        content_type = self.headers.get('Content-Type', '')
        if content_type.startswith('multipart/'):
            message = BytesParser(policy=default).parsebytes(
                ('Content-Type: '+content_type+'\r\n\r\n').encode()+data)
            body = {part.get_param('name', header='content-disposition'):
                    json.loads(part.get_payload(decode=True))
                    for part in message.iter_parts()}
        else:
            body = json.loads(data)
        with (root/'requests.jsonl').open('a') as output:
            output.write(json.dumps({'path':self.path, 'body':body})+'\n')
        mode = (root/'mode').read_text().strip() if (root/'mode').exists() else 'success'
        self.reply(503 if mode == 'failure' else 200, {'error':'fixture unavailable'} if mode == 'failure' else
                   {'namespace':'cai.messages','queue':'cai.fixture',
                    'message_id':'fixture-message','attempts':0,'max_attempts':3,
                    'failure_attempts':0,'not_visible_until_unix':0,
                    'visibility_timeout_seconds':30,'payload_bytes':0}, 'fixture-lockd')
    def do_GET(self):
        self.reply(503, {'error':'metadata unavailable'}, 'fixture-metadata')
    def reply(self, status, value, correlation):
        payload=json.dumps(value).encode()
        self.send_response(status)
        self.send_header('Content-Type','application/json')
        self.send_header('Content-Length',str(len(payload)))
        self.send_header('X-Correlation-Id',correlation)
        self.end_headers()
        self.wfile.write(payload)
server=http.server.HTTPServer(('127.0.0.1',0),Peer)
if len(sys.argv)>2:
    context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(root/'server.crt',root/'server.key')
    context.load_verify_locations(root/'ca.crt')
    context.verify_mode=ssl.CERT_REQUIRED
    server.socket=context.wrap_socket(server.socket,server_side=True)
(root/'port').write_text(str(server.server_port))
server.serve_forever(poll_interval=.05)
