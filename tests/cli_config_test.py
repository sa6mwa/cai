#!/usr/bin/env python3
"""Config boot/template, env/CLI precedence, persistence and redacted errors."""
import json
import os
import pathlib
import stat
import subprocess
import sys
import tempfile
sys.dont_write_bytecode = True
from cli_terminal_test import Responses


def main():
    cli = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(dir=cli.parent, prefix='config-') as directory:
        root = pathlib.Path(directory)
        work = root / 'work'
        work.mkdir()
        api = Responses()
        api.headers = []
        env = {k: v for k, v in os.environ.items() if not k.startswith('LOG_') and
               k.lower() not in ('http_proxy', 'https_proxy', 'all_proxy')}
        for key in ('CAI_API_KEY', 'BERGET_API_KEY', 'OPENAI_API_KEY'):
            env.pop(key, None)
        env.update(HOME=str(root), XDG_CONFIG_HOME=str(root/'config'),
                   XDG_STATE_HOME=str(root/'state'), XDG_CACHE_HOME=str(root/'cache'),
                   XDG_DATA_HOME=str(root/'data'), NO_PROXY='127.0.0.1,localhost',
                   no_proxy='127.0.0.1,localhost')
        path = root/'config'/'cai'/'config.ini'
        args = [str(cli), '-C', str(work)]

        def run(extra=(), selected=env, stdin=None):
            return subprocess.run(args + list(extra), env=selected, input=stdin,
                                  capture_output=True, text=True, timeout=12)

        def contents(precedence=False):
            return ('config_env_has_precedence = ' + str(precedence).lower() + '\n'
                    '[client]\nprovider = custom\nbase_url = "http://127.0.0.1:' +
                    str(api.server_port) + '/v1"\napi_key_env = CAI_API_KEY\n'
                    '[agent]\nmodel = configured-model\nreasoning_effort = high\n'
                    'terminal = false\nimage_generation = false\n'
                    '[env]\nCAI_API_KEY = "config-key-secret"\n')

        try:
            result = run(['--help'])
            assert result.returncode == 0 and '--config' in result.stdout
            assert not path.exists(), 'help created configuration'
            result = run(['--version'])
            assert result.returncode == 0 and not path.exists()
            result = run(['--no-config', '--list'])
            assert result.returncode == 0 and not path.exists(), result.stderr
            result = run(['--list'])
            assert result.returncode == 0, result.stderr
            template = path.read_text()
            assert stat.S_IMODE(path.stat().st_mode) == 0o600
            assert stat.S_IMODE(path.parent.stat().st_mode) == 0o700
            assert '# model = "gpt-6-luna"' in template and '[env]' in template
            assert '# config_env_has_precedence = false' in template
            assert '[response]' not in template and '[session_limits]' not in template
            assert 'Choices: smith|smith-review' not in template
            assert '# preset = "smith"' in template
            assert all(not line.strip() or line.startswith('#') or line.startswith('[')
                       for line in template.splitlines()), template
            path.write_text(contents())
            api.plan = ['ConfigTurnOne']
            result = run(['-Nni', 'first configured request'])
            assert result.returncode == 0 and 'ConfigTurnOne' in result.stdout, result.stderr
            assert path.read_text() == contents(), 'startup rewrote user configuration'
            assert api.requests[-1]['model'] == 'configured-model'
            assert api.requests[-1]['reasoning']['effort'] == 'high'
            assert 'config-key-secret' not in result.stderr + result.stdout
            # Config environment reaches the normal pslog configuration before startup.
            path.write_text(contents() + 'LOG_LEVEL = warn\n')
            api.plan = ['QuietFromConfig']
            result = run(['-Nni', 'quiet configured logging'])
            assert result.returncode == 0 and 'QuietFromConfig' in result.stdout, result.stderr
            assert 'quiet configured logging' not in result.stderr
            selected = env.copy()
            selected['LOG_LEVEL'] = 'info'
            api.plan = ['NormalLogEnvironmentWins']
            result = run(['-Nni', 'visible configured logging'], selected)
            assert result.returncode == 0, result.stderr
            records = [json.loads(line) for line in result.stderr.splitlines()]
            assert any(r.get('lvl') == 'info' and r.get('role') == 'user' for r in records)
            path.write_text(contents())
            selected = env.copy()
            selected['CAI_API_KEY'] = 'normal-env-key'
            api.plan = ['EnvPriorityDone']
            result = run(['-Nni', 'normal environment'], selected)
            assert result.returncode == 0, result.stderr
            assert api.headers[-1]['Authorization'] == 'Bearer normal-env-key'
            path.write_text(contents(True))
            api.plan = ['ConfigEnvPriorityDone']
            result = run(['-Nni', 'config environment'], selected)
            assert result.returncode == 0, result.stderr
            assert api.headers[-1]['Authorization'] == 'Bearer config-key-secret'
            api.plan = ['CliOverridesDone']
            result = run(['-Nni', 'explicit flags', '-m', 'cli-model', '-r', 'low'])
            assert result.returncode == 0, result.stderr
            assert api.requests[-1]['model'] == 'cli-model'
            assert api.requests[-1]['reasoning']['effort'] == 'low'
            # Same-directory resume preserves saved selections against file defaults.
            api.plan = ['SavedSettingsDone']
            result = run(['-ni', 'resume previous'])
            assert result.returncode == 0, result.stderr
            assert api.requests[-1]['model'] == 'cli-model' and api.requests[-1]['reasoning']['effort'] == 'low'
            # A CLI provider selector wins over the file provider without
            # creating a spurious --endpoint/custom validation conflict.
            path.write_text(contents().replace('model = configured-model', 'model = ""')
                            .replace('reasoning_effort = high', 'reasoning_effort = ""'))
            api.plan = ['ProviderOverrideAutomaticDefaults']
            result = run(['-Nni', 'provider override', '-p', 'openai'])
            assert result.returncode == 0, result.stderr
            assert api.requests[-1]['model'] == 'gpt-6-luna'
            assert api.requests[-1]['reasoning']['effort'] == 'medium'
            path.write_text(contents())
            before = len(api.requests)
            for bad in ('[agent]\nunknown_field=config-key-secret\n',
                        '[env]\nCAI_API_KEY="config-key-secret\n',
                        '[client]\ntimeout_ms=-9\n',
                        '[client]\nprovider=""\n',
                        '[agent]\npreset=smith-review\n',
                        '[response]\nmax_output_tokens=100\n',
                        '[runtime]\nrecord_transcript=false\n'):
                path.write_text(bad)
                result = run(['-Nni', 'never submitted'])
                assert result.returncode != 0
                assert 'config.ini:' in result.stderr
                assert 'config-key-secret' not in result.stderr + result.stdout
            assert len(api.requests) == before
            path.write_text(contents())
            # Explicit file selection, file-relative paths and disabled loading.
            alternate = root/'alternate.ini'
            alternate.write_text(contents() + '[paths]\nexport_dir = "exports"\n')
            api.plan = ['AlternateDone']
            result = run(['--config', str(alternate), '-Nni', 'alternate config'])
            assert result.returncode == 0, result.stderr
            listing = run(['--list'])
            identifier = listing.stdout.splitlines()[0].split()[0]
            result = run(['--config', str(alternate), '--export', identifier])
            assert result.returncode == 0, result.stderr
            assert (root / 'exports' / identifier / (identifier + '.jsonl')).exists()
            # -C resolves before file-relative --config; configured directories
            # resolve against the file and determine session scope when -C is absent.
            path.write_text(contents() + '[paths]\ndirectory = "../.. /bad"\n')
            api.plan = ['ExplicitDirectoryWins']
            result = run(['-Nni', 'explicit directory wins'])
            assert result.returncode == 0, result.stderr
            path.write_text(contents())
            local = work / 'local.ini'
            local.write_text(contents())
            api.plan = ['RelativeConfigAfterChdir']
            result = run(['--config', 'local.ini', '-Nni', 'relative selection'])
            assert result.returncode == 0, result.stderr
            alternate.write_text(contents() + '[paths]\ndirectory = "work"\n')
            api.plan = ['ConfiguredDirectory']
            result = subprocess.run([str(cli), '--config', str(alternate), '-Nni', 'configured directory'],
                                    cwd=root, env=env, capture_output=True, text=True, timeout=12)
            assert result.returncode == 0, result.stderr
            result = run(['--no-config', '--list'])
            assert result.returncode == 0, result.stderr
            assert path.read_text() == contents()
            selected = env.copy()
            selected.pop('XDG_CONFIG_HOME')
            result = run(['--list'], selected)
            assert result.returncode == 0, result.stderr
            assert (root/'.config'/'cai'/'config.ini').exists()
            custom_dir = root / 'custom-config'
            result = run(['--config-dir', str(custom_dir), '--list'])
            assert result.returncode == 0, result.stderr
            assert (custom_dir / 'config.ini').exists()
            # Explicit relative --config-dir remains anchored to the directory
            # in which it selected config.ini, even if that file changes cwd.
            anchored = root / 'anchored-config'
            anchored.mkdir()
            (anchored / 'config.ini').write_text(contents() + '[paths]\ndirectory = "../work"\n')
            (anchored / 'AGENTS.md').write_text('Follow REVIEW_CONFIG_ROOT_SENTINEL.\n')
            api.plan = ['AnchoredInstructions']
            result = subprocess.run([str(cli), '--config-dir', 'anchored-config', '-Nni', 'anchored instructions'],
                                    cwd=root, env=env, capture_output=True, text=True, timeout=12)
            assert result.returncode == 0, result.stderr
            assert 'REVIEW_CONFIG_ROOT_SENTINEL' in json.dumps(api.requests[-1])
            # Restrictive umask must not make new configuration directories or
            # templates unusable; existing directories are left as they are.
            strict = root / 'strict-config' / 'cai' / 'config.ini'
            result = subprocess.run(['/bin/sh', '-c', 'umask 222; exec "$@"', 'config-test',
                                     *args, '--config', str(strict), '--list'],
                                    env=env, capture_output=True, text=True, timeout=12)
            assert result.returncode == 0, result.stderr
            assert stat.S_IMODE(strict.stat().st_mode) == 0o600
            assert stat.S_IMODE(strict.parent.stat().st_mode) == 0o700
            result = run(['--no-config', '--config', str(alternate), '--list'])
            assert result.returncode == 2 and 'mutually exclusive' in result.stderr
        finally:
            api.shutdown()
            api.server_close()
            api.thread.join()


if __name__ == '__main__':
    main()
