#!/usr/bin/env python3
"""Real terminal shell ownership, restoration, guards and failure recovery."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import termios

sys.dont_write_bytecode = True
from cli_terminal_test import Responses, Terminal


def main():
    cli = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(dir=cli.parent, prefix='handoff-') as directory:
        root = pathlib.Path(directory)
        work = root / 'work'
        work.mkdir()
        launches = root / 'launches'
        shell = root / 'user shell'
        shell.write_text('''#!/bin/sh
if [ "$1" != -i ]; then exec /bin/sh "$@"; fi
test -t 0 && test -t 1 || exit 77
printf x >> "$CAI_TEST_SHELL_LAUNCHES"
printf 'SHELL_HANDOFF_READY\\n'
exec /bin/sh -i
''')
        shell.chmod(0o700)
        api = Responses()
        env = {k: v for k, v in os.environ.items()
               if not k.startswith('LOG_') and k.lower() not in
               ('http_proxy', 'https_proxy', 'all_proxy', 'no_color')}
        env.update(HOME=str(root), XDG_STATE_HOME=str(root / 'state'),
                   XDG_CACHE_HOME=str(root / 'cache'), XDG_CONFIG_HOME=str(root / 'config'),
                   XDG_DATA_HOME=str(root / 'data'), CAI_API_KEY='fixture', SHELL=str(shell),
                   CAI_TEST_SHELL_LAUNCHES=str(launches),
                   NO_PROXY='127.0.0.1,localhost', no_proxy='127.0.0.1,localhost')
        args = [str(cli), '-N', '-p', 'custom', '--endpoint',
                f'http://127.0.0.1:{api.server_port}/v1', '-m', 'gpt-5.6-luna',
                '--no-terminal', '--no-image-generation', '-C', str(work)]
        terminal = None

        def start(selected=env):
            instance = Terminal(args, selected, controlling_tty=True)
            instance.wait(lambda: instance.contains('Welcome to CAI'))
            return instance

        def tty_canonical():
            flags = termios.tcgetattr(terminal.master)[3]
            return bool(flags & termios.ICANON) and bool(flags & termios.ECHO)

        def count():
            return len(launches.read_text()) if launches.exists() else 0

        def enter(text):
            before = count()
            terminal.output.clear()
            terminal.send(text.encode() + b'\r')
            terminal.wait(lambda: count() == before + 1 and terminal.contains('SHELL_HANDOFF_READY'))
            assert tty_canonical(), 'shell inherited raw input'

        def returned(marker='Welcome to CAI'):
            terminal.wait(lambda: marker.encode() in b' '.join(terminal.rendered().split())
                          and not tty_canonical())
            assert os.tcgetpgrp(terminal.master) == terminal.process.pid

        try:
            terminal = start()
            enter('/shell')
            terminal.resize(48)
            terminal.send(b"touch ../normal-permissions; printf '\033[2J\033[H'; "
                          b"printf 'UNSHARED_TERMINAL_OUTPUT'; exit\r")
            returned()
            assert (root / 'normal-permissions').exists(), 'shell was sandboxed'
            assert not api.requests, 'handoff started inference'
            api.plan = ['POST HANDOFF ANSWER\n\n' + 'word ' * 20 + '\n\n']
            terminal.output.clear()
            terminal.send(b'continue after the handoff\r')
            terminal.wait(lambda: terminal.contains('POST HANDOFF ANSWER') and terminal.contains('Worked for'))
            request = json.dumps(api.requests[-1])
            assert 'UNSHARED_TERMINAL_OUTPUT' not in request and '<user_shell_command>' not in request
            assert '/shell' not in request
            rows = [row for row in terminal.rendered().splitlines()
                    if row.startswith(b'  word')]
            assert len(rows) >= 2 and all(len(row) <= 48 for row in rows), rows

            # Ctrl-R Enter selects an editable shell command; only the next
            # Enter launches it. History survives the terminal handoff.
            before = count()
            terminal.output.clear()
            terminal.send(b'\x12/shell\r')
            terminal.wait(lambda: terminal.contains('/shell'))
            assert count() == before and not tty_canonical()
            terminal.send(b'\r')
            terminal.wait(lambda: count() == before + 1 and tty_canonical())
            terminal.send(b'exit 7\r')
            returned('interactive shell exited 7')
            terminal.send(b'\x15')
            enter('!sh')
            terminal.send(b'kill -KILL $$\r')
            returned('interactive shell terminated by signal 9')

            # Busy submissions are kept editable, never queued or sent to the
            # model, including Tab and Alt-Enter queue gestures.
            before = count()
            api.release.clear()
            api.pause_script = True
            api.plan = ['BusyHandoffDone']
            terminal.output.clear()
            terminal.send(b'busy turn\r')
            terminal.wait(lambda: terminal.contains('STREAM in flight'))
            terminal.output.clear()
            terminal.send(b'/shell\r')
            terminal.wait(lambda: terminal.contains('Shell is available only between turns'))
            assert count() == before and not tty_canonical()
            terminal.send(b'\t\x1b\r')
            terminal.wait(lambda: terminal.contains('Use Enter to open the shell'))
            assert count() == before
            terminal.send(b'\x15')
            api.release.set()
            terminal.wait(lambda: terminal.contains('BusyHandoffDone') and terminal.contains('Worked for'))
            assert '/shell' not in json.dumps(api.requests[-1])
            terminal.close()
            terminal = None

            # A missing executable returns to CAI, without interpreting SHELL
            # as command syntax. Standard fallback shells remain interactive.
            selected = env.copy()
            selected['SHELL'] = str(root / 'missing; touch injected')
            terminal = start(selected)
            terminal.output.clear()
            terminal.send(b'/shell\r')
            returned('interactive shell exited 127')
            assert not (work / 'injected').exists()
            terminal.close()
            terminal = None
            for value in (None, ''):
                selected = env.copy()
                if value is None:
                    selected.pop('SHELL')
                else:
                    selected['SHELL'] = value
                terminal = start(selected)
                terminal.output.clear()
                terminal.send(b'!sh\r')
                terminal.wait(tty_canonical)
                terminal.send(b'touch fallback-shell; exit\r')
                returned()
                assert (work / 'fallback-shell').exists()
                (work / 'fallback-shell').unlink()
                terminal.close()
                terminal = None

            for command in ('/shell', '!sh'):
                before = count()
                result = subprocess.run(args, input=command + '\n/quit\n', env=env,
                                        text=True, capture_output=True, timeout=10)
                assert result.returncode == 0, result.stderr
                assert 'interactive shell requires terminal input and output' in result.stdout + result.stderr
                assert count() == before
            records = []
            for path in (root / 'cache' / 'cai').glob('*.log'):
                records.extend(json.loads(row) for row in path.read_text().splitlines())
            assert any(row.get('event') == 'terminal_shell_started' for row in records)
            assert any(row.get('event') == 'terminal_shell_started' and row.get('shell') == str(shell)
                       for row in records)
            assert any(row.get('event') == 'terminal_shell_completed' and row.get('exit_code') == 7
                       for row in records)
        finally:
            api.release.set()
            if terminal is not None:
                if terminal.process.poll() is None:
                    terminal.process.kill()
                terminal.process.communicate()
                os.close(terminal.master)
            api.shutdown()
            api.server_close()
            api.thread.join()


if __name__ == '__main__':
    main()
