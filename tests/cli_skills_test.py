#!/usr/bin/env python3
"""Skill installation, failure atomicity, catalog injection and resource tools."""
import hashlib
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
    with tempfile.TemporaryDirectory(dir=cli.parent, prefix='skills-') as temporary:
        root = pathlib.Path(temporary)
        work = root / 'workspace'
        work.mkdir()
        source = root / 'source-package'
        (source / 'scripts').mkdir(parents=True)
        (source / 'references').mkdir()
        (source / 'references' / 'example-package').mkdir()
        (source / 'references' / 'example-package' / 'SKILL.md').write_text(
            '---\nname: artifact-not-a-skill\ndescription: Nested example.\n---\n')
        text = ('---\nname: "installed-example"\ndescription: >-\n'
                '  Help with the installed\n  package resources.\nmetadata:\n'
                '  category: [test, example]\n---\nBODY_ONLY_ON_DEMAND\n'
                'Read scripts/run.sh using read_skill.\n')
        (source / 'SKILL.md').write_text(text)
        script = source / 'scripts' / 'run.sh'
        script.write_text('#!/bin/sh\n# SCRIPT_RESOURCE_SENTINEL\nexit 0\n')
        script.chmod(0o755)
        binary = bytes(range(256)) * 8192
        (source / 'references' / 'binary.dat').write_bytes(binary)
        (source / '.artifact').write_text('hidden artifact')
        env = {key: value for key, value in os.environ.items()
               if not key.startswith('LOG_') and key.lower() not in
               ('http_proxy', 'https_proxy', 'all_proxy')}
        for key in ('OPENAI_API_KEY', 'OPENROUTER_API_KEY', 'BERGET_API_KEY', 'CAI_API_KEY'):
            env.pop(key, None)
        env.update(HOME=str(root), XDG_CONFIG_HOME=str(root / 'config'),
                   XDG_STATE_HOME=str(root / 'state'), XDG_CACHE_HOME=str(root / 'cache'),
                   XDG_DATA_HOME=str(root / 'data'), NO_PROXY='127.0.0.1,localhost',
                   no_proxy='127.0.0.1,localhost')
        installed_root = root / 'config' / 'cai' / 'skills'
        installed = installed_root / 'installed-example'

        def run(*arguments):
            prefix = ['valgrind', '--error-exitcode=91', '--leak-check=no'] if (
                os.environ.get('CAI_SKILLS_TEST_VALGRIND') == '1' and '-Nni' in arguments) else []
            return subprocess.run([*prefix, str(cli), '-C', str(work), *map(str, arguments)],
                                  env=env, capture_output=True, text=True, timeout=15)

        result = run('--install-skill', source)
        assert result.returncode == 0 and 'Installed skill installed-example' in result.stdout, result.stderr
        assert not (root / 'state').exists(), 'installer opened session/auth storage'
        assert (installed / 'SKILL.md').read_text() == text
        assert stat.S_IMODE((installed / 'scripts' / 'run.sh').stat().st_mode) == 0o700
        assert hashlib.sha256((installed / 'references' / 'binary.dat').read_bytes()).digest() == hashlib.sha256(binary).digest()
        assert (installed / '.artifact').read_text() == 'hidden artifact'
        aliases = installed_root / 'collection' / 'manual-package'
        aliases.mkdir(parents=True)
        (aliases / 'SKILL.md').write_text('---\nname: duplicate-name\ndescription: Existing alias.\n---\n')
        (source / 'SKILL.md').write_text('---\nname: duplicate-name\ndescription: A conflicting name.\n---\n')
        result = run('--install-skill', source)
        assert result.returncode != 0 and 'another installed package' in result.stderr
        assert not (installed_root / 'duplicate-name').exists()
        (source / 'SKILL.md').write_text(text)
        assert not list(installed_root.glob('.skill-install-*'))
        result = run('--install-skill', source)
        assert result.returncode != 0 and '--replace' in result.stderr and not result.stdout
        (installed / 'obsolete').write_text('old-only artifact')
        script.write_text('#!/bin/sh\n# SCRIPT_RESOURCE_SENTINEL updated\nexit 0\n')
        result = run('--install-skill', source, '--replace')
        assert result.returncode == 0 and 'Replaced skill' in result.stdout, result.stderr
        assert not (installed / 'obsolete').exists()
        assert (installed / 'scripts' / 'run.sh').read_text() == script.read_text()
        previous = (installed / 'SKILL.md').read_bytes()
        # A failing copy must not replace the installed package or expose staging.
        (source / 'bad-link').symlink_to(root / 'outside')
        result = run('--install-skill', source, '--replace')
        assert result.returncode != 0 and 'symlinks' in result.stderr
        assert (installed / 'SKILL.md').read_bytes() == previous
        assert not list(installed_root.glob('.skill-install-*'))
        (source / 'bad-link').unlink()
        os.mkfifo(source / 'bad-fifo')
        result = run('--install-skill', source, '--replace')
        assert result.returncode != 0 and (installed / 'SKILL.md').read_bytes() == previous
        (source / 'bad-fifo').unlink()
        result = run('--install-skill', source, '--skills-dir', source / 'inside')
        assert result.returncode != 0 and not (source / 'inside').exists()
        result = run('--install-skill', installed, '--replace')
        assert result.returncode != 0 and 'same directory' in result.stderr
        for arguments in (('--replace',), ('--install-skill', source, '--list'),
                          ('--install-skill', source, '-ni', 'must not run'),
                          ('--install-skill', source, '--review')):
            result = run(*arguments)
            assert result.returncode == 2, (arguments, result.stdout, result.stderr)
        for bad in ('---\nname: invalid\ndescription: no closing\n',
                    '---\nname: ../escape\ndescription: Bad.\n---\n',
                    '---\nname: .hidden\ndescription: Bad.\n---\n',
                    '---\nname: broken\ndescription: [invalid]\n---\n'):
            (source / 'SKILL.md').write_text(bad)
            result = run('--install-skill', source, '--replace')
            assert result.returncode != 0 and not result.stdout
            assert (installed / 'SKILL.md').read_bytes() == previous
        (source / 'SKILL.md').write_text(text)
        source_link = root / 'source-link'
        source_link.symlink_to(source, target_is_directory=True)
        result = run('--install-skill', source_link)
        assert result.returncode != 0
        custom = root / 'custom-skills'
        result = run('--install-skill', source, '--skills-dir', custom)
        assert result.returncode == 0 and (custom / 'installed-example' / 'SKILL.md').exists(), result.stderr
        # Installation must resolve exactly the same path as runtime discovery,
        # including ordinary symlink ancestors followed by '..'.
        physical = root / 'physical'
        (physical / 'child').mkdir(parents=True)
        ancestor = root / 'ancestor-link'
        ancestor.symlink_to(physical / 'child', target_is_directory=True)
        selected = str(ancestor) + '/../skills'
        result = run('--install-skill', source, '--skills-dir', selected)
        assert result.returncode == 0, result.stderr
        assert (physical / 'skills' / 'installed-example' / 'SKILL.md').exists()
        assert not (root / 'skills').exists()
        into_source = root / 'into-source'
        into_source.symlink_to(source, target_is_directory=True)
        result = run('--install-skill', source, '--skills-dir', into_source / 'new-root')
        assert result.returncode != 0 and not (source / 'new-root').exists()
        final_link = root / 'skills-root-link'
        final_link.symlink_to(custom, target_is_directory=True)
        result = run('--install-skill', source, '--skills-dir', final_link, '--replace')
        assert result.returncode != 0
        config_root = root / 'alternate-config'
        result = run('--install-skill', source, '--config-dir', config_root)
        assert result.returncode == 0 and (config_root / 'skills' / 'installed-example' / 'SKILL.md').exists(), result.stderr
        # Real runtime turn: catalog is developer context, full instructions and
        # artifacts are read on demand, and escaping the package is denied.
        broken = installed_root / 'invalid-package'
        broken.mkdir()
        (broken / 'SKILL.md').write_text('not front matter\n')
        api = Responses()
        env['CAI_API_KEY'] = 'fixture'
        env['LOG_LEVEL'] = 'warn'
        try:
            api.plan = [('read_skill', {'skill': 'installed-example'}),
                        ('read_skill', {'skill': 'installed-example', 'resource': 'scripts/run.sh'}),
                        ('read_skill', {'skill': 'installed-example', 'resource': '../../escape'}),
                        'SkillsRuntimeDone']
            result = run('-Nni', 'Use installed-example', '-p', 'custom', '--endpoint',
                         f'http://127.0.0.1:{api.server_port}/v1', '-m', 'gpt-6-luna',
                         '--no-terminal', '--no-image-generation', '--no-review-subagent')
            assert result.returncode == 0 and 'SkillsRuntimeDone' in result.stdout, (result.returncode, result.stderr)
            first = json.dumps(api.requests[0])
            assert 'installed-example: Help with the installed package resources.' in first
            assert 'BODY_ONLY_ON_DEMAND' not in first
            assert 'artifact-not-a-skill' not in first
            assert 'BODY_ONLY_ON_DEMAND' in json.dumps(api.requests[1])
            assert 'SCRIPT_RESOURCE_SENTINEL' in json.dumps(api.requests[2])
            assert 'skill resource path is invalid' in json.dumps(api.requests[3])
            assert 'invalid-package' in result.stderr and 'front matter' in result.stderr
        finally:
            api.shutdown()
            api.server_close()
            api.thread.join()


if __name__ == '__main__':
    main()
