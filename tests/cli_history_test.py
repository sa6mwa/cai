#!/usr/bin/env python3
"""Workspace-keyed native recall: immediate writes, replay and storage failures."""
import hashlib
import json
import os
import pathlib
import re
import stat
import sys
import tempfile
import time

sys.dont_write_bytecode = True
from cli_terminal_test import Responses, Terminal


def main():
    cli = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(dir=cli.parent, prefix='history-') as directory:
        root = pathlib.Path(directory)
        work = root / 'work'
        other = root / 'other'
        work.mkdir()
        other.mkdir()
        api = Responses()
        env = {k: v for k, v in os.environ.items()
               if not k.startswith('LOG_') and k.lower() not in
               ('http_proxy', 'https_proxy', 'all_proxy', 'no_color')}
        env.update(HOME=str(root), XDG_STATE_HOME=str(root / 'state'),
                   XDG_CACHE_HOME=str(root / 'cache'), XDG_CONFIG_HOME=str(root / 'config'),
                   XDG_DATA_HOME=str(root / 'data'), CAI_API_KEY='fixture',
                   NO_PROXY='127.0.0.1,localhost', no_proxy='127.0.0.1,localhost')
        base = [str(cli), '-p', 'custom', '--endpoint',
                f'http://127.0.0.1:{api.server_port}/v1', '-m', 'gpt-5.6-luna',
                '--no-terminal', '--no-image-generation']
        history_dir = root / 'state' / 'cai' / 'history'
        history = history_dir / (hashlib.sha256(str(work.resolve()).encode()).hexdigest() + '.history')
        terminal = None

        def start(workspace=work, selected=env, fresh=False):
            instance = Terminal(base + ['-C', str(workspace)] + (['-N'] if fresh else []), selected)
            instance.wait(lambda: instance.contains('Welcome to CAI'))
            return instance

        def command(text, marker):
            terminal.output.clear()
            terminal.send(text.encode() + b'\r')
            terminal.wait(lambda: terminal.contains(marker))

        def failed():
            deadline = time.monotonic() + 8
            while terminal.process.poll() is None and time.monotonic() < deadline:
                terminal.read(0.05)
            assert terminal.process.poll() not in (None, 0), terminal.rendered()
            terminal.read(0)
            _, errors = terminal.process.communicate(timeout=1)
            result = terminal.rendered() + (errors or b'')
            assert b'history' in result.lower(), result
            os.close(terminal.master)

        try:
            terminal = start(fresh=True)
            api.release.clear()
            api.pause_script = True
            api.plan = ['HistoryFirstDone', 'HistorySteerDone']
            prompt = 'history unicode λ\nsecond \\ tab\t'
            terminal.send(b'\x1b[200~' + prompt.encode() + b'\x1b[201~\r')
            terminal.wait(lambda: terminal.contains('STREAM in flight'))
            expected = 'history unicode λ\\nsecond \\\\ tab\\t\n'
            assert history.read_text() == expected, history.read_text()
            assert stat.S_IMODE(history.stat().st_mode) == 0o600
            assert stat.S_IMODE(history_dir.stat().st_mode) == 0o700
            assert not (root / 'state' / 'softline').exists()
            original_id = next((root / 'cache' / 'cai').glob('*.log')).stem
            terminal.send(b'history-steering\r')
            terminal.wait(lambda: 'history-steering\n' in history.read_text())
            assert history.read_text() == expected + 'history-steering\n'
            api.release.set()
            terminal.wait(lambda: terminal.contains('HistorySteerDone') and terminal.contains('Worked for'))
            assert history.read_text() == expected + 'history-steering\n', 'steer delivery appended twice'
            command('/new', '--- session ---')
            before = history.read_bytes()
            requests = len(api.requests)
            terminal.output.clear()
            terminal.send(b'\x12history-steering\r')
            terminal.wait(lambda: b'> history-steering' in terminal.rendered())
            assert len(api.requests) == requests and history.read_bytes() == before
            terminal.send(b'\x15')
            terminal.close()
            terminal = None
            before = history.read_bytes()
            terminal = start()
            assert history.read_bytes() == before, 'startup replay persisted prompts again'
            terminal.output.clear()
            terminal.send(b'\x12history-steering\r')
            terminal.wait(lambda: b'> history-steering' in terminal.rendered())
            assert len(api.requests) == requests
            terminal.send(b'\x15')
            command('/resume', original_id)
            row = re.search(rb'(?m)^\s*(\d+)\s+' + original_id.encode() + rb'\s', terminal.rendered())
            assert row is not None, terminal.rendered()
            selected_command = '/resume ' + row[1].decode()
            before = history.read_bytes()
            command(selected_command, 'HistorySteerDone')
            assert history.read_bytes() == before + selected_command.encode() + b'\n', 'replay re-appended input'
            assert len(api.requests) == requests
            terminal.close()
            terminal = None
            terminal = start(other, fresh=True)
            before = len(api.requests)
            terminal.send(b'\x10/goal\r')
            terminal.wait(lambda: terminal.contains('No goal.'))
            assert len(api.requests) == before, 'recall leaked across workspaces'
            other_history = history_dir / (hashlib.sha256(str(other.resolve()).encode()).hexdigest() + '.history')
            assert other_history.read_text() == '/goal\n'
            terminal.close()
            terminal = None

            # Truncated final records are ignored, then repaired under the
            # native append lock before accepting the next prompt.
            with history.open('ab') as file:
                file.write(b'unfinished-tail')
            terminal = start(fresh=True)
            command('/goal', 'No goal.')
            assert b'unfinished-tail' not in history.read_bytes()
            terminal.close()
            terminal = None

            # Unsafe storage aborts before model activity, with useful native
            # error details. Append failures cannot submit an unsaved prompt.
            before = len(api.requests)
            backup = history.with_suffix(".original")
            history.rename(backup)
            history.symlink_to(backup)
            terminal = Terminal(base + ['-N', '-C', str(work)], env)
            failed()
            terminal = None
            history.unlink()
            backup.rename(history)
            terminal = start(fresh=True)
            snapshot = history.read_bytes()
            history.rename(backup)
            history.symlink_to(backup)
            terminal.send(b'unsaved prompt must not reach inference\r')
            failed()
            terminal = None
            assert history.read_bytes() == snapshot
            assert len(api.requests) == before
            history.unlink()
            backup.rename(history)

            # The native default is overridden even when XDG_STATE_HOME is
            # absent; CAI's HOME fallback remains separate from Softline's.
            selected = env.copy()
            selected.pop('XDG_STATE_HOME')
            terminal = start(selected=selected, fresh=True)
            command('/goal', 'No goal.')
            fallback = root / '.local' / 'state' / 'cai' / 'history' / history.name
            assert fallback.read_text() == '/goal\n'
            assert not (root / '.local' / 'state' / 'softline').exists()
            terminal.close()
            terminal = None
        finally:
            api.release.set()
            if terminal is not None:
                if terminal.process.poll() is None:
                    terminal.process.kill()
                terminal.process.communicate()
                try:
                    os.close(terminal.master)
                except OSError:
                    pass
            api.shutdown()
            api.server_close()
            api.thread.join()


if __name__ == '__main__':
    main()
