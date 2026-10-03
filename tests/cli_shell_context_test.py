#!/usr/bin/env python3
"""Shell context joins both provider protocols after tools, with no extra turn."""
import http.server
import json
import os
import pathlib
import sys
import tempfile
import threading

sys.dont_write_bytecode = True
from cli_terminal_test import Responses, Terminal


class ChatPeer(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        self.server.requests.append(request)
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.end_headers()

        def event(delta, reason=None):
            self.wfile.write(('data: ' + json.dumps({
                'id': 'chat-shell', 'object': 'chat.completion.chunk',
                'choices': [{'index': 0, 'delta': delta, 'finish_reason': reason}]
            }) + '\n\n').encode())
            self.wfile.flush()

        first = len(self.server.requests) == 1
        if first:
            event({'role': 'assistant', 'content': 'STREAM in flight\n\n'})
            assert self.server.release.wait(10)
            event({'tool_calls': [{'index': 0, 'id': 'call_read', 'type': 'function',
                                   'function': {'name': 'read_file',
                                                'arguments': '{"path":"data.txt"}'}}]})
            event({}, 'tool_calls')
        else:
            event({'role': 'assistant', 'content': 'BOUNDARY done\n\n'})
            event({}, 'stop')
        self.wfile.write(b'data: [DONE]\n\n')
        self.wfile.flush()


def main():
    cli, fixture = (pathlib.Path(arg).resolve() for arg in sys.argv[1:])
    for chat, binary in ((False, cli), (True, fixture)):
        with tempfile.TemporaryDirectory(dir=cli.parent, prefix='shell-context-') as directory:
            root = pathlib.Path(directory)
            work = root / 'work'
            work.mkdir()
            (work / 'data.txt').write_text('tool-output-marker\n')
            env = {k: v for k, v in os.environ.items()
                   if not k.startswith('LOG_') and k.lower() not in
                   ('http_proxy', 'https_proxy', 'all_proxy', 'no_color')}
            env.update(HOME=str(root), XDG_STATE_HOME=str(root / 'state'),
                       XDG_CACHE_HOME=str(root / 'cache'), XDG_CONFIG_HOME=str(root / 'config'),
                       CAI_API_KEY='fixture', SHELL='/bin/sh', NO_PROXY='127.0.0.1,localhost',
                       no_proxy='127.0.0.1,localhost')
            if chat:
                api = http.server.HTTPServer(('127.0.0.1', 0), ChatPeer)
                api.requests = []
                api.release = threading.Event()
                api.thread = threading.Thread(target=api.serve_forever,
                                              kwargs={'poll_interval': 0.01}, daemon=True)
                api.thread.start()
            else:
                api = Responses()
                api.pause_script = True
                api.plan = [('read_file', {'path': 'data.txt'}), 'BOUNDARY done']
            args = [str(binary), '-N', '-p', 'custom', '--endpoint',
                    f'http://127.0.0.1:{api.server_port}/v1', '-m', 'gpt-5.6-luna',
                    '--no-terminal', '--no-image-generation', '-C', str(work)]
            terminal = None
            try:
                terminal = Terminal(args, env)
                terminal.wait(lambda: terminal.contains('Welcome to CAI'))
                terminal.send(b'begin tool boundary test\r')
                terminal.wait(lambda: terminal.contains('STREAM in flight'))
                terminal.send(b'!printf shell-boundary-marker\r')
                terminal.wait(lambda: terminal.contains('Shell command exited 0'))
                assert len(api.requests) == 1
                assert 'shell-boundary-marker' not in json.dumps(api.requests[0])
                api.release.set()
                terminal.wait(lambda: terminal.contains('BOUNDARY done') and
                              terminal.contains('Worked for'))
                assert len(api.requests) == 2, api.requests
                request = api.requests[1]
                items = request['messages' if chat else 'input']
                shell_indexes = [i for i, item in enumerate(items)
                                 if '<user_shell_command>' in json.dumps(item)]
                assert len(shell_indexes) == 1
                tool_indexes = [i for i, item in enumerate(items)
                                if item.get('role') == 'tool' or
                                item.get('type') == 'function_call_output']
                assert tool_indexes and tool_indexes[-1] < shell_indexes[0], items
                assert 'shell-boundary-marker' in json.dumps(items[shell_indexes[0]])
                assert items[shell_indexes[0]]['role'] == 'user'
                assert 'tool-output-marker' in json.dumps(request)
                terminal.close()
                terminal = None
            finally:
                api.release.set()
                if terminal is not None:
                    terminal.close()
                api.shutdown()
                api.server_close()
                api.thread.join()


if __name__ == '__main__':
    main()
