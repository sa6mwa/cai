#!/usr/bin/env python3
"""Exercise literal warning wrapping and local notices through real renderers."""
import json
import os
import pathlib
import re
import sys
import tempfile
import unicodedata
sys.dont_write_bytecode = True
from cli_terminal_test import Terminal

ANSI = rb"\x1b\[[0-?]*[ -/]*[@-~]"
MESSAGE = "literal **stars** _path_ `tick` [name](url) café λ &amp; " + " ".join(
    f"word_{n:02}" for n in range(1, 21)) + " END_LITERAL"


def main():
    cli = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(dir=cli.parent, prefix="notices-") as temp:
        root = pathlib.Path(temp)
        terminal = None
        try:
            for color, width in [(True, 40), (False, 72)]:
                case = root / str(width)
                case.mkdir()
                env = {k: v for k, v in os.environ.items()
                       if not k.startswith("LOG_") and k.lower() not in
                       ("no_color", "http_proxy", "https_proxy", "all_proxy")}
                env.update(HOME=str(case), XDG_STATE_HOME=str(case / "state"),
                           XDG_CACHE_HOME=str(case / "cache"), XDG_CONFIG_HOME=str(case / "config"),
                           XDG_DATA_HOME=str(case / "data"), CAI_API_KEY="fixture",
                           NO_PROXY="127.0.0.1,localhost", no_proxy="127.0.0.1,localhost",
                           CAI_TEST_NOTICE_MESSAGE=MESSAGE)
                if not color:
                    env["NO_COLOR"] = "1"
                args = [str(cli), "-N", "-p", "custom", "--endpoint",
                        "http://127.0.0.1:1/v1", "-m", "gpt-5.6-luna",
                        "--no-image-generation", "-C", str(case)]
                terminal = Terminal(args, env, width=width)
                terminal.wait(lambda: terminal.contains("Welcome to CAI"))
                terminal.wait(lambda: terminal.contains("END_LITERAL"))
                rendered = terminal.rendered()
                assert b"* WARN:" in rendered, rendered
                assert b"**stars**" in rendered and b"_path_" in rendered, rendered
                assert b"`tick`" in rendered and b"[name](url)" in rendered, rendered
                assert "café λ".encode() in rendered and b"&amp;" in rendered, rendered
                assert re.search(rb"\r?\n  word_\d+", rendered), rendered
                notice_text = rendered.decode("utf-8").split("! Welcome", 1)[0]
                rows = [row for row in notice_text.splitlines() if row]
                assert rows[0].startswith("* WARN:") and len(rows) > 1, rows
                for i, row in enumerate(rows):
                    columns = sum(2 if unicodedata.east_asian_width(ch) in "WF"
                                  else 0 if unicodedata.combining(ch) else 1 for ch in row)
                    assert columns <= width, (width, columns, row)
                    if i:
                        assert row.startswith("  "), row
                assert (b"\x1b[91m*\x1b[0m" in terminal.output) == color
                before = len(terminal.output)
                terminal.send(b"/goal\r")
                terminal.wait(lambda: b"manage one." in b" ".join(
                    re.sub(ANSI, b"", terminal.output[before:]).split()))
                local = re.sub(ANSI, b"", terminal.output[before:])
                assert b"No goal." in local and b"\n" in local, local
                terminal.close()
                terminal = None
                records = []
                for log in (case / "cache" / "cai").glob("*.log"):
                    records.extend(json.loads(line) for line in log.read_text().splitlines())
                assert any(MESSAGE in str(record) for record in records), records
        finally:
            if terminal is not None:
                if terminal.process.poll() is None:
                    terminal.process.kill()
                terminal.process.communicate()
                os.close(terminal.master)


if __name__ == "__main__":
    main()
