#!/usr/bin/env python3
"""Loopback provider wire fixture. Plans are supplied by shell/compiled tests."""
import http.server
import json
from pathlib import Path
import sys
import time
import traceback

root = Path(sys.argv[1])

class Peer(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.0'

    def log_message(self, *_args):
        pass

    def do_GET(self):
        config = json.loads((root / 'config.json').read_text())
        with (root / 'gets').open('a') as output:
            output.write(self.path + '\n')
        self.send_response(config.get('catalog_status', 200))
        self.send_header('Content-Type', 'application/json')
        self.end_headers()
        self.wfile.write(json.dumps(config.get('catalog', {'data': [
            {'id': 'gpt-5.6-luna', 'name': 'Luna'},
            {'id': 'gpt-6-astra', 'name': 'Astra'}]})).encode())

    def event(self, kind, **values):
        value = dict(type=kind, **values)
        self.wfile.write(('event: ' + kind + '\ndata: ' + json.dumps(value) + '\n\n').encode())
        self.wfile.flush()

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers.get('Content-Length', 0))))
        self.server.sequence += 1
        with (root / 'requests.jsonl').open('a') as output:
            output.write(json.dumps(request) + '\n')
        with (root / 'headers.jsonl').open('a') as output:
            output.write(json.dumps(dict(self.headers)) + '\n')
        config = json.loads((root / 'config.json').read_text())
        if request.get('method') == 'initialize':
            payload = {'result': {'serverInfo': {'name': 'fixture'}}} if config.get('mcp_ready', True) else {'error': 'not ready'}
            self.send_response(200)
            self.end_headers()
            text = json.dumps(payload)
            if config.get('mcp_sse'):
                text = 'data: \n\nevent: message\ndata: ' + text + '\n\n'
            self.wfile.write(text.encode())
            return
        item = config.get('plan', ['unexpected request']).pop(0) if config.get('plan') else 'unexpected request'
        (root / 'config.json').write_text(json.dumps(config))
        if item == '__transport_failure__':
            self.close_connection = True
            return
        if item in ('__api_error__', '__http_error__'):
            status = 429 if item == '__api_error__' else 500
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('x-request-id', 'req_fixture_429')
            self.end_headers()
            self.wfile.write(json.dumps({'error': {'message': 'quota exhausted' if status == 429 else 'fixture error',
                'type': 'rate_limit_error', 'code': 'rate_limit_exceeded'}}).encode())
            return
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        if isinstance(item, dict) and item.get('truncated'):
            self.send_header('Content-Length', '10000')
        self.end_headers()
        if isinstance(item, str):
            aliases = {'__complete_goal__': {'tool': 'update_goal', 'arguments': {'status': 'complete'}},
                       '__run_review__': {'tool': 'run_review', 'arguments': {'target': 'uncommitted'}},
                       '__read_file__': {'tool': 'read_file', 'arguments': {'path': 'tracked.txt'}},
                       '__summary__': {'reasoning': ['Short provider summary'], 'text': 'summary reply'},
                       '__partial_failure__': {'text': 'partial answer', 'error': 'stream failed'}}
            item = aliases.get(item, {'text': item})
        if isinstance(item, list):
            item = {'deltas': item}
        if item.get('malformed'):
            self.wfile.write(b'event: response.output_text.delta\ndata: {bad json}\n\n')
            return
        chat = config.get('chat', False)
        def delta(value, finish=None):
            self.wfile.write(('data: ' + json.dumps({'id': 'chat-fixture', 'object': 'chat.completion.chunk',
                'choices': [{'index': 0, 'delta': value, 'finish_reason': finish}]}) + '\n\n').encode())
            self.wfile.flush()
        for summary in item.get('reasoning', []):
            if chat: delta({'reasoning_content': summary})
            else: self.event('response.reasoning_summary_text.delta', delta=summary)
        if item.get('pause'):
            if chat: delta({'role': 'assistant', 'content': 'STREAM in flight\n\n'})
            else: self.event('response.output_text.delta', delta='STREAM in flight\n\n')
            deadline = time.monotonic() + 15
            while not (root / 'release').exists() and time.monotonic() < deadline:
                time.sleep(.01)
            if not (root / 'release').exists():
                return
        if 'tool' in item:
            arguments = item.get('arguments', {})
            call = item.get('call_id', 'call_fixture_' + str(self.server.sequence))
            if chat:
                delta({'tool_calls': [{'index': 0, 'id': call, 'type': 'function',
                    'function': {'name': item['tool'], 'arguments': json.dumps(arguments)}}]})
            else:
                tool = dict(id='fc_fixture', type='function_call', call_id=call, name=item['tool'], arguments=json.dumps(arguments))
                if isinstance(arguments, str):
                    tool.pop('arguments')
                    tool.update(type='custom_tool_call', input=arguments)
                self.event('response.output_item.done', output_index=0, item=tool)
        for text in item.get('deltas', [item['text']] if 'text' in item else []):
            if chat: delta({'role': 'assistant', 'content': text})
            else: self.event('response.output_text.delta', delta=text)
        if item.get('truncated'):
            return
        if item.get('error'):
            self.event('error', code='fixture_error', message=item['error'])
        elif chat:
            delta({}, 'tool_calls' if 'tool' in item else 'stop')
            self.wfile.write(b'data: [DONE]\n\n')
        else:
            self.event('response.completed', response={'id': 'resp_fixture_' + str(time.monotonic_ns()),
                'usage': {'input_tokens': 10, 'output_tokens': 10, 'total_tokens': 20}})

class Server(http.server.HTTPServer):
    def handle_error(self, request, client_address):
        if not isinstance(sys.exc_info()[1], (BrokenPipeError, ConnectionResetError)):
            traceback.print_exc()

server = Server(('127.0.0.1', 0), Peer)
server.sequence = 0
(root / 'port').write_text(str(server.server_port))
try:
    server.serve_forever(poll_interval=.05)
finally:
    server.server_close()
