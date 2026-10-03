#!/bin/sh
set -eu
if [ "$#" -ne 1 ]; then echo "usage: $0 <repo-root>" >&2; exit 2; fi
exec python3 "$1/tests/devenv_test.py" "$1" ownership
