#!/usr/bin/env python3
"""Exercise real Make recipes with isolated, recorded build/test operations."""
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


repo = Path(sys.argv[1]).resolve()
make = shutil.which("make")


def executable(path, source):
    path.write_text(source)
    path.chmod(0o755)


with tempfile.TemporaryDirectory(prefix="lifecycle-test-", dir=repo / "build") as tmp:
    root = Path(tmp)
    (root / "scripts").mkdir()
    (root / "bin").mkdir()
    log = root / "events.tsv"
    record = root / "bin" / "record"
    executable(record, """#!/bin/sh
printf '%s\\t' "$@" >>"$CAI_LIFECYCLE_LOG"
printf '\\n' >>"$CAI_LIFECYCLE_LOG"
if [ "${CAI_LIFECYCLE_FAIL:-}" = "$1" ]; then exit 7; fi
""")
    for command in ("cmake", "ctest"):
        executable(root / "bin" / command,
                   f'#!/bin/sh\nexec "{record}" {command} "$@"\n')
    for script in ("build.sh", "test.sh", "package.sh", "package-verify.sh"):
        executable(root / "scripts" / script,
                   f'#!/bin/sh\nexec "{record}" {script} "$@"\n')
    executable(root / "scripts" / "release_version.sh",
               '#!/bin/sh\nprintf "0.0.0\\n"\n')
    fixture_makefile = root / "Makefile"
    (root / "include/cai").mkdir(parents=True)
    (root / "src").mkdir()
    leaves = ("build-debug", "build-cli", "build-release", "build-host",
              "valgrind", "fuzz-smoke", "lua-test", "test-offline", "test-e2e",
              "release-lua-artifacts", "format", "require-clean-worktree",
              "lifecycle-version-contract", "clean", "clean-dist",
              "package-source-smoke")
    source = (repo / "Makefile").read_text()
    for target in leaves:
        source, count = re.subn(
            rf"^{re.escape(target)}:[^\n]*\n(?:[ \t][^\n]*\n|\n)*",
            lambda _: f'{target}:\n\t@"{record}" {target}\n\n', source, flags=re.M)
        assert count == 1, target
    fixture_makefile.write_text(source)
    env = {k: v for k, v in os.environ.items()
           if k not in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "CTEST_FILTER", "CTEST_FLAGS")}
    env.update(PATH=f'{root / "bin"}:{os.environ["PATH"]}',
               CAI_LIFECYCLE_LOG=str(log))
    # Recursive make uses the same fixture; production orchestration recipes execute.
    recursive = f'{make} -s -f {fixture_makefile}'

    def run(target, fail=""):
        log.write_text("")
        result = subprocess.run(
            [make, "-s", "-j8", "-f", str(fixture_makefile),
             f"MAKE={recursive}"] + ([target] if isinstance(target, str) else list(target)), cwd=root,
            env=dict(env, CAI_LIFECYCLE_FAIL=fail), capture_output=True, text=True)
        events = [line.split("\t")[:-1] for line in log.read_text().splitlines()]
        assert (result.returncode != 0) == bool(fail), (target, result.stderr)
        return events

    for target in ("test", "test-debug", "finalize-slice"):
        events = run(target)
        assert sum(event[:2] == ["test.sh", "debug"] for event in events) == 1
        assert not any(event[0] in ("build-release", "package.sh") for event in events)
        selection = next(event for event in events if event[0] == "test.sh")
        assert selection[-4:] == ["-L", "unit", "-LE", "live|packaging"], selection

    events = run(("test", "test-debug"))
    assert sum(e[:2] == ["test.sh", "debug"] for e in events) == 1, events
    events = run("test-cli")
    assert [event[0] for event in events] == ["build-cli", "test.sh"], events
    assert events[-1][1] == "debug-cai"
    events = run("test-all")
    assert [event[0] for event in events] == [
        "build-debug", "test.sh", "build-cli", "test.sh", "valgrind",
        "fuzz-smoke", "lua-test", "test-offline", "test-e2e"], events
    assert [event[1] for event in events if event[0] == "test.sh"] == ["debug", "debug-cai"]
    assert next(e for e in events if e[:2] == ["test.sh", "debug"])[-2:] == ["-LE", "live"]

    def assert_matrix(events):
        names = [event[0] for event in events]
        expected = ["clean-dist", "build-release", "ctest", "valgrind",
                    "fuzz-smoke", "lua-test", "test-offline", "test-e2e",
                    "build.sh", "test.sh", "package.sh", "cmake",
                    "release-lua-artifacts", "cmake", "package-verify.sh"]
        assert names == expected, events
        assert events[2][1:3] == ["--test-dir", "build/x86_64-linux-gnu-release"]
        assert events[8] == ["build.sh", "cai"]
        assert events[9][1] == "cai"
        assert "live" in events[2]
        assert "--parallel" in events[2] and "1" in events[2]

    assert_matrix(run("release-matrix"))
    for target in ("prerelease", "release-pipeline"):
        events = run(target)
        assert [event[0] for event in events[:2]] == ["format", "require-clean-worktree"]
        assert_matrix(events[2:])
    events = run("release")
    assert [event[0] for event in events[:4]] == [
        "lifecycle-version-contract", "clean", "format", "require-clean-worktree"]
    assert_matrix(events[4:-3])
    assert [event[0] for event in events[-3:]] == [
        "package-source-smoke", "cmake", "package-verify.sh"]

    for failed in ("build-release", "ctest", "valgrind", "fuzz-smoke", "lua-test",
                   "test-offline", "test-e2e", "build.sh", "test.sh"):
        events = run("release-matrix", failed)
        assert events[-1][0] == failed, events
        assert not any(event[0] == "package.sh" for event in events), events
    for failed in ("lifecycle-version-contract", "clean", "format", "require-clean-worktree"):
        events = run("release", failed)
        assert events[-1][0] == failed and not any(e[0] == "build-release" for e in events)
    for failed in ("package.sh", "cmake", "package-verify.sh"):
        events = run("release-matrix", failed)
        assert events[-1][0] == failed, events

    # The test driver must not rebuild; Make owns that step.
    shutil.copyfile(repo / "scripts/test.sh", root / "scripts/test.sh")
    for mode in ("debug", "debug-cai", "cai", "release", "integration"):
        log.write_text("")
        subprocess.run(["bash", str(root / "scripts/test.sh"), mode], env=env, check=True)
        events = [line.split("\t")[:-1] for line in log.read_text().splitlines()]
        assert events == [["ctest", "--preset", mode]], events

    # Matching compiler collections reuse configuration; changed collections refresh it.
    shutil.copyfile(repo / "scripts/build.sh", root / "scripts/build.sh")
    executable(root / "scripts/cpkt-toolchains.sh",
               '#!/bin/sh\nprintf "root=/pinned/current\\n"\n')
    executable(root / "scripts/osxcross_available.sh", '#!/bin/sh\nexit 1\n')
    for stale in (False, True):
        for target in ("x86_64-linux-gnu", "x86_64-linux-musl", "aarch64-linux-gnu",
                       "aarch64-linux-musl", "armhf-linux-gnu", "armhf-linux-musl"):
            build = root / "build" / f"{target}-release"
            build.mkdir(parents=True, exist_ok=True)
            (build / "CMakeCache.txt").write_text(
                f'CMAKE_TOOLCHAIN_FILE:FILEPATH={root}/cmake/toolchains/linux-bootlin.cmake\n'
                f'CPKT_TARGET_ID:STRING={target}\n'
                f'CPKT_BOOTLIN_ROOT:PATH=/pinned/{"old" if stale else "current"}\n'
                'CMAKE_C_COMPILER:FILEPATH=/pinned/current/bin/gcc\n')
        log.write_text("")
        subprocess.run(["bash", str(root / "scripts/build.sh"), "release-matrix"],
                       env=env, check=True, capture_output=True)
        events = [line.split("\t")[:-1] for line in log.read_text().splitlines()]
        assert len(events) == 12, events
        for configure, build in zip(events[::2], events[1::2]):
            assert ("--fresh" in configure) == stale, configure
            assert build[1:3] == ["--build", "--preset"], build

    executable(root / "scripts/cpkt-aflpp.sh", '#!/bin/sh\nexit 0\n')
    for mode, target in (("debug-cai", "cai_cli_tests"), ("cai", "cai_cli_tests"),
                         ("fuzz", "cai_fuzzers"), ("valgrind", "cai_tests")):
        log.write_text("")
        subprocess.run(["bash", str(root / "scripts/build.sh"), mode],
                       env=env, check=True, capture_output=True)
        events = [line.split("\t")[:-1] for line in log.read_text().splitlines()]
        assert events[-1] == ["cmake", "--build", "--preset", mode, "--target", target], events

    # CLI artifacts join the manifest and have a separate static-binary policy.
    package = root / "cli-package"
    docs = ("share/man/man1/cai.1", "share/doc/libcai/README.md",
            "share/doc/libcai/LICENSE", "share/doc/libcai/docs/model-metadata.md",
            "share/doc/libcai/docs/chrome.md")
    for doc in docs:
        path = package / doc
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("public documentation\n")
    (package / "bin").mkdir()
    binary = package / "bin/cai"
    executable(binary, "static fixture\n")
    readelf = root / "readelf"
    executable(readelf, """#!/bin/sh
if [ "${CAI_ELF_FIXTURE:-}" = unreadable ]; then exit 1; fi
if [ "$1" = -h ]; then
  if [ "${CAI_ELF_FIXTURE:-}" = wrong-arch ]; then
    printf 'Class: ELF32\\nMachine: ARM\\n'
  else
    printf 'Class: ELF64\\nMachine: Advanced Micro Devices X86-64\\n'
  fi
else
  case "${CAI_ELF_FIXTURE:-}" in
    loader) printf ' INTERP 0x0\\n';;
    dynamic) printf ' DYNAMIC 0x0\\n';;
    *) printf ' LOAD 0x0\\n';;
  esac
fi
""")

    def cli_gate(kind=""):
        return subprocess.run(
            ["bash", "-c", 'source "$1" --self-test; verify_cli_archive "$2"',
             "test", str(repo / "scripts/verify_release_artifacts.sh"), str(package)],
            env=dict(os.environ, CAI_REPO_ROOT=str(repo), CAI_VERSION="1.2.3",
                     CAI_READELF=str(readelf), CAI_ELF_FIXTURE=kind),
            capture_output=True, text=True)

    assert cli_gate().returncode == 0
    for kind in ("wrong-arch", "loader", "dynamic", "unreadable"):
        result = cli_gate(kind)
        assert result.returncode != 0 and "CLI" in result.stderr, result.stderr
    binary.chmod(0o644)
    assert cli_gate().returncode != 0
    binary.chmod(0o755)
    for doc in docs:
        path = package / doc
        path.unlink()
        assert cli_gate().returncode != 0, doc
        path.write_text("public documentation\n")
    binary.unlink()
    binary.symlink_to(package / docs[0])
    assert cli_gate().returncode != 0

    dist = root / "dist"
    dist.mkdir()
    archive = "cai-cli-1.2.3-x86_64-linux-musl.tar.gz"
    (dist / archive).write_bytes(b"checksum fixture")
    subprocess.run([shutil.which("cmake"), f"-DCAI_DIST_DIR={dist}", "-DCAI_VERSION=1.2.3",
                    "-P", str(repo / "cmake/package_checksums.cmake")], check=True,
                   capture_output=True)
    assert archive in (dist / "cai-1.2.3-CHECKSUMS").read_text()

    # Editor checks fail promptly, including a checker that ignores SIGTERM.
    editor = root / "editor"
    editor.mkdir()
    (editor / "compile_commands.json").write_text(json.dumps([
        {"file": str(editor / "probe.c"), "command": "cc -c probe.c", "directory": str(editor)}]))
    clangd = root / "clangd"
    for mode in ("pass", "fail", "hang"):
        executable(clangd, """#!/usr/bin/env python3
import os, signal, sys, time
mode = os.environ['CAI_CLANGD_FIXTURE']
if mode == 'fail': sys.exit(4)
if mode == 'hang':
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    time.sleep(30)
""")
        result = subprocess.run(
            [shutil.which("cmake"), f"-DCAI_SOURCE_DIR={repo}", f"-DCAI_BUILD_DIR={editor}",
             f"-DCAI_CLANGD_BIN={clangd}", "-DCAI_CLANGD_TIMEOUT=1", "-P",
             str(repo / "cmake/clangd_check.cmake")],
            env=dict(os.environ, CAI_CLANGD_FIXTURE=mode), capture_output=True, text=True, timeout=5)
        assert (result.returncode == 0) == (mode == "pass"), result.stderr
        if mode != "pass":
            assert "probe.c" in result.stderr, result.stderr

print("Native development, one-pass release gates, fail-stop ordering and cache reuse verified")
