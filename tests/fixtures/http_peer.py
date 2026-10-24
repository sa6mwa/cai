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
        try:
            time.sleep(config.get('catalog_delay_ms', 0) / 1000)
            self.send_response(config.get('catalog_status', 200))
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            self.wfile.write(json.dumps(config.get('catalog', {'data': [
                {'id': 'gpt-5.6-luna', 'name': 'Luna'},
                {'id': 'gpt-6-astra', 'name': 'Astra'}]})).encode())
        finally:
            (root / 'catalog_finished').write_text(str(time.monotonic_ns()))

    def event(self, kind, **values):
        value = dict(type=kind, **values)
        self.wfile.write(('event: ' + kind + '\ndata: ' + json.dumps(value) + '\n\n').encode())
        self.wfile.flush()

    def do_POST(self):
        if self.headers.get('Transfer-Encoding', '').lower() == 'chunked':
            chunks = []
            while True:
                length = int(self.rfile.readline().split(b';')[0], 16)
                if length == 0:
                    while self.rfile.readline().strip():
                        pass
                    break
                chunks.append(self.rfile.read(length))
                self.rfile.read(2)
            body = b''.join(chunks)
        else:
            body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        request = json.loads(body)
        if self.path.startswith('/mcp/'):
            return self.mcp(request)
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
        if isinstance(item, dict) and item.get('http_status', 200) >= 400:
            self.send_response(item['http_status'])
            self.send_header('Content-Type', 'application/json')
            self.send_header('x-request-id', item.get('error_request_id', 'req_policy_http'))
            self.end_headers()
            self.wfile.write(json.dumps({'error': {'code': item['error_code'], 'message': item['error']}}).encode())
            return
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        if isinstance(item, dict) and item.get('error_request_id'):
            self.send_header('x-request-id', item['error_request_id'])
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
        for index, part in enumerate(item.get('reasoning_parts', [])):
            for summary in part:
                self.event('response.reasoning_summary_text.delta',
                           summary_index=index, delta=summary)
            self.event('response.reasoning_summary_part.done', summary_index=index)
        if item.get('pause'):
            marker = item.get('pause_text', 'STREAM in flight\n\n')
            if chat: delta({'role': 'assistant', 'content': marker})
            else: self.event('response.output_text.delta', delta=marker)
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
            error = dict(code=item.get('error_code', 'fixture_error'), message=item['error'])
            if chat:
                self.wfile.write(('data: ' + json.dumps({'error': error}) + '\n\n').encode())
            elif item.get('error_event') == 'response.failed':
                self.event('response.failed', response={'error': error})
            else:
                self.event('error', **error)
        elif chat:
            delta({}, 'tool_calls' if 'tool' in item else 'stop')
            if config.get('usage'):
                self.wfile.write(('data: ' + json.dumps({'id': 'chat-fixture', 'choices': [],
                    'usage': config['usage']}) + '\n\n').encode())
            self.wfile.write(b'data: [DONE]\n\n')
        else:
            self.event('response.completed', response={'id': 'resp_fixture_' + str(time.monotonic_ns()),
                'usage': {'input_tokens': 10, 'output_tokens': 10, 'total_tokens': 20}})

    def mcp(self, request):
        config = json.loads((root / 'config.json').read_text())
        name = self.path.split('/')[-1]
        spec = config.get('mcp', {}).get(name, {})
        method = request.get('method')
        with (root / 'mcp_requests.jsonl').open('a') as output:
            output.write(json.dumps(dict(server=name, request=request,
                                          authorization=self.headers.get('Authorization'),
                                          time_ns=time.monotonic_ns())) + '\n')
        time.sleep(spec.get('delay_ms', 0) / 1000 if method != 'notifications/initialized' else 0)
        self.send_response(spec.get('status', 202 if method == 'notifications/initialized' else 200))
        notify = method == 'tools/call' and spec.get('notify_tools_changed')
        self.send_header('Content-Type', 'text/event-stream' if notify else 'application/json')
        self.end_headers()
        if method == 'notifications/initialized':
            return
        if spec.get('malformed'):
            self.wfile.write(b'{broken')
            return
        if method == 'initialize':
            result = dict(protocolVersion=request['params']['protocolVersion'],
                          capabilities={'tools': {}}, serverInfo={'name': name, 'version': '1'})
        elif method == 'tools/list':
            page = 1 if request.get('params', {}).get('cursor') else 0
            tools = [dict(name='echo', description='Echo | user text\nplain',
                          inputSchema={'type': 'object', 'properties': {'text': {'type': 'string'}},
                                       'required': ['text'], 'additionalProperties': False})] if page == 0 else [
                dict(name='raw', description='Accept raw text',
                     inputSchema={'type': 'object', 'properties': {'payload': {'type': 'string'}},
                                  'required': ['payload'], 'additionalProperties': False},
                     _meta={'systems.c89.cai/raw-input': {'field': 'payload', 'framing': 'text'}})]
            result = dict(tools=tools)
            if page == 0 and not spec.get('empty'):
                result['nextCursor'] = 'second-page'
            if spec.get('empty'):
                result['tools'] = []
            if 'tools' in spec:
                result = dict(tools=spec['tools'])
        elif method == 'tools/call':
            time.sleep(spec.get('call_delay_ms', 0) / 1000)
            result = dict(content=[{'type': 'text', 'text': 'MCP TOOL RESULT ' + json.dumps(request['params'])}])
        else:
            result = {}
        response = dict(jsonrpc='2.0', id=request.get('id'), result=result)
        if notify:
            notice = dict(jsonrpc='2.0', method='notifications/tools/list_changed')
            self.wfile.write(('data: ' + json.dumps(notice) + '\n\n' +
                              'data: ' + json.dumps(response) + '\n\n').encode())
        else:
            self.wfile.write(json.dumps(response).encode())

class Server(http.server.ThreadingHTTPServer):
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
