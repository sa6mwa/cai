#!/usr/bin/env python3
"""Model/effort controls, safe boundaries and real pouch session recovery."""
import http.server
import json
import os
import pathlib
import re
import sys
import tempfile
import threading

sys.dont_write_bytecode = True
from cli_terminal_test import ResponsePeer, Responses, Terminal


class CatalogPeer(ResponsePeer):
    def do_GET(self):
        self.server.gets.append(self.path)
        assert self.path == '/v1/models?client_version=0.160.0', self.path
        self.send_response(self.server.catalog_status)
        self.send_header('Content-Type', 'application/json')
        self.end_headers()
        self.wfile.write(json.dumps({'models': self.server.models}).encode())


class ChatPeer(CatalogPeer):
    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        self.server.requests.append(request)
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.end_headers()

        def event(delta, reason=None):
            self.wfile.write(('data: ' + json.dumps({
                'id': 'chat-settings', 'object': 'chat.completion.chunk',
                'choices': [{'index': 0, 'delta': delta, 'finish_reason': reason}]
            }) + '\n\n').encode())
            self.wfile.flush()

        plan = self.server.plan.pop(0) if self.server.plan else 'FINAL paragraph.'
        if isinstance(plan, tuple):
            event({'role': 'assistant', 'content': 'STREAM in flight\n\n'})
            assert self.server.release.wait(10)
            name, args = plan
            event({'tool_calls': [{'index': 0, 'id': 'call_settings', 'type': 'function',
                                  'function': {'name': name, 'arguments': json.dumps(args)}}]})
            event({}, 'tool_calls')
        else:
            event({'role': 'assistant', 'content': plan + '\n\n'})
            event({}, 'stop')
        self.wfile.write(b'data: [DONE]\n\n')
        self.wfile.flush()


def model(slug, efforts):
    return {'slug': slug, 'display_name': slug, 'context_window': 272000,
            'supported_in_api': True, 'default_reasoning_level': 'medium',
            'supported_reasoning_levels': [{'effort': effort} for effort in efforts]}


def main():
    cli = pathlib.Path(sys.argv[1]).resolve()
    for chat in (False, True):
        with tempfile.TemporaryDirectory(dir=cli.parent, prefix='settings-') as directory:
            root = pathlib.Path(directory)
            work = root / 'work'
            work.mkdir()
            (work / 'data.txt').write_text('settings tool output\n')
            api = Responses()
            api.RequestHandlerClass = ChatPeer if chat else CatalogPeer
            api.gets = []
            api.models = [model('gpt-6-luna', ['low', 'medium', 'high', 'max']),
                          model('gpt-6.1-sol', ['low', 'medium', 'high', 'xhigh', 'max', 'ultra']),
                          model('no-reasoning', [])]
            env = {k: v for k, v in os.environ.items()
                   if not k.startswith('LOG_') and k.lower() not in
                   ('http_proxy', 'https_proxy', 'all_proxy', 'no_color')}
            env.update(HOME=str(root), XDG_STATE_HOME=str(root / 'state'),
                       XDG_CACHE_HOME=str(root / 'cache'), XDG_CONFIG_HOME=str(root / 'config'),
                       XDG_DATA_HOME=str(root / 'data'), OPENAI_API_KEY='fixture',
                       OPENROUTER_API_KEY='fixture', NO_PROXY='127.0.0.1,localhost',
                       no_proxy='127.0.0.1,localhost',
                       CAI_TEST_API_BASE_URL=f'http://127.0.0.1:{api.server_port}/v1')
            if chat:
                env['CAI_TEST_CHAT_COMPLETIONS'] = '1'
            args = [str(cli), '-p', 'openai', '--no-terminal', '--no-image-generation',
                    '-C', str(work)]
            terminal = None

            def command(text, expected):
                terminal.output.clear()
                terminal.send(text.encode() + b'\r')
                terminal.wait(lambda: terminal.contains(expected))

            def prompt(text, response):
                api.plan = [response]
                before = len(api.requests)
                terminal.output.clear()
                terminal.send(text.encode() + b'\r')
                terminal.wait(lambda: len(api.requests) > before and terminal.contains(response)
                              and terminal.contains('Worked for'))

            def selection(request):
                return request['model'], (request.get('reasoning_effort') if chat
                                          else request.get('reasoning', {}).get('effort'))

            try:
                terminal = Terminal(args + ['-N'], env)
                terminal.wait(lambda: terminal.contains('Welcome to CAI'))
                command('/help', 'manual.')
                assert terminal.contains('/reasoning')
                assert terminal.contains('/model')
                command('/reasoning', 'max')
                assert terminal.contains('Provider-advertised choices')
                assert terminal.contains('high') and not terminal.contains('ultra')
                assert not api.requests
                prompt('original settings conversation', 'INITIAL settings response')
                assert selection(api.requests[-1]) == ('gpt-6-luna', 'medium')
                command('/model gpt-6.1-sol high', 'Model changed to gpt-6.1-sol; reasoning effort high')
                command('/reasoning', 'ultra')
                assert b'$ /reasoning' in terminal.rendered()
                command('/reasoning xhigh', 'Reasoning effort changed to xhigh')
                command('/model low', 'Reasoning effort changed to low')
                command('/model gpt-6-luna', 'Model changed to gpt-6-luna')
                command('/model', 'Reasoning effort: low')
                assert terminal.contains('low')
                count = len(api.requests)
                command('/reasoning xhigh', 'does not advertise reasoning effort xhigh')
                command('/reasoning invalid', 'unknown reasoning effort')
                command('/reasoning high extra', 'use /reasoning EFFORT')
                command('/model gpt-6.1-sol invalid', 'use /model MODEL')
                command('/model gpt-6.1-sol high extra', 'use /model MODEL')
                assert len(api.requests) == count, 'settings started inference'
                command('/model  gpt-6.1-sol  high', 'Model changed to gpt-6.1-sol')
                api.pause_script = True
                api.release.clear()
                api.plan = [('read_file', {'path': 'data.txt'}), 'BOUNDARY settings response']
                terminal.output.clear()
                terminal.send(b'use a tool while switching settings\r')
                terminal.wait(lambda: terminal.contains('STREAM in flight'))
                assert selection(api.requests[-1]) == ('gpt-6.1-sol', 'high')
                count = len(api.requests)
                command('/model gpt-6-luna low', 'Model gpt-6-luna; reasoning effort low queued')
                command('/reasoning high', 'Reasoning effort high queued')
                command('/model', 'Pending reasoning: high')
                assert terminal.contains('Pending:') and terminal.contains('gpt-6-luna')
                command('/reasoning', 'max')
                assert terminal.contains('Choices for gpt-6-luna')
                assert not terminal.contains('ultra'), 'choices used the old effective model'
                assert len(api.requests) == count
                api.release.set()
                terminal.wait(lambda: terminal.contains('BOUNDARY settings response')
                              and terminal.contains('Worked for'))
                assert selection(api.requests[-1]) == ('gpt-6-luna', 'high')
                assert 'settings tool output' in json.dumps(api.requests[-1])
                assert '/reasoning' not in json.dumps(api.requests[-1])
                api.pause_script = False
                command('/model gpt-6.1-sol ultra', 'reasoning effort ultra')
                terminal.close()
                terminal = None
                session_id = next((root / 'cache' / 'cai').glob('*.log')).stem
                terminal = Terminal(args, env)
                terminal.wait(lambda: terminal.contains('Welcome to CAI'))
                command('/model', 'Reasoning effort: ultra')
                assert re.search(rb'Current:\s*gpt-6\.1-sol', terminal.rendered()), terminal.rendered()
                assert re.search(rb'Reasoning effort:\s*ultra', terminal.rendered()), terminal.rendered()
                prompt('after resume', 'RESUMED settings response')
                assert selection(api.requests[-1]) == ('gpt-6.1-sol', 'ultra')
                assert 'original settings conversation' in json.dumps(api.requests[-1])
                command('/new', '--- session ---')
                command('/model', 'Reasoning effort: medium')
                assert re.search(rb'Current:\s*gpt-6-luna', terminal.rendered())
                assert re.search(rb'Reasoning effort:\s*medium', terminal.rendered())
                prompt('new settings conversation', 'NEW settings response')
                before = len(api.requests)
                command('/resume', 'original settings conversation')
                row = re.search(rb'(?m)^\s*(\d+)\s+' + session_id.encode() + rb'\s', terminal.rendered())
                assert row is not None, terminal.rendered()
                command('/resume ' + row[1].decode(), '--- session ---')
                command('/model', 'Reasoning effort: ultra')
                assert re.search(rb'Current:\s*gpt-6\.1-sol', terminal.rendered())
                assert len(api.requests) == before, 'resume sent inference'
                # Exact startup resume restores the original session independently
                # of the freshly opened default session.
                terminal.close()
                terminal = Terminal(args + ['--resume', session_id], env)
                terminal.wait(lambda: terminal.contains('Welcome to CAI'))
                command('/model', 'Reasoning effort: ultra')
                assert re.search(rb'Current:\s*gpt-6\.1-sol', terminal.rendered())
                assert re.search(rb'Reasoning effort:\s*ultra', terminal.rendered())
                terminal.close()
                terminal = Terminal(args + ['--resume', session_id, '-m', 'gpt-6-luna', '-r', 'low'], env)
                terminal.wait(lambda: terminal.contains('Welcome to CAI'))
                command('/model', 'Reasoning effort: low')
                assert re.search(rb'Current:\s*gpt-6-luna', terminal.rendered())
                assert re.search(rb'Reasoning effort:\s*low', terminal.rendered())
                command('/model no-reasoning', 'Model changed to no-reasoning')
                command('/reasoning', 'no reasoning-effort choices')
                command('/reasoning high', 'does not advertise reasoning effort high')
                command('/model unknown-model', 'Model changed to unknown-model')
                command('/reasoning', 'Model-specific reasoning choices are unavailable')
                command('/reasoning high', 'Reasoning effort changed to high')
                terminal.close()
                terminal = Terminal([str(cli), '-N', '-p', 'openrouter', '--no-terminal',
                                     '--no-image-generation', '-C', str(work)], env)
                terminal.wait(lambda: terminal.contains('Welcome to CAI'))
                gets = len(api.gets)
                command('/model', "OpenRouter's large catalog is not listed")
                assert len(api.gets) == gets, 'OpenRouter catalog was fetched'
                assert not terminal.contains('no-reasoning')
                terminal.close()
                api.models = [model(f'catalog-model-{i:02}', ['low']) for i in range(70)]
                terminal = Terminal(args + ['-N'], env)
                terminal.wait(lambda: terminal.contains('Welcome to CAI'))
                command('/model', 'Showing 50 of 70 models')
                assert terminal.contains('catalog-model-49')
                assert not terminal.contains('catalog-model-50'), 'large catalog flooded the feed'
                terminal.close()
                api.catalog_status = 500
                terminal = Terminal(args + ['-N'], env)
                terminal.wait(lambda: terminal.contains('Welcome to CAI'))
                command('/reasoning', 'Model-specific reasoning choices are unavailable')
                command('/model gpt-6.1-sol high', 'Model changed to gpt-6.1-sol')
                prompt('use selected settings without a catalog', 'OUTAGE settings response')
                assert selection(api.requests[-1]) == ('gpt-6.1-sol', 'high')
                terminal.close()
                terminal = None
                print(('Chat Completions' if chat else 'Responses') + ': settings and recovery passed')
            finally:
                api.release.set()
                if terminal is not None:
                    terminal.close()
                api.shutdown()
                api.server_close()
                api.thread.join()


if __name__ == '__main__':
    main()
