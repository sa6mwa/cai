#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
cat >"$root/models.json" <<'JSON'
{"models":[{"provider":"custom","id":"gpt-5.6-luna","reasoning_efforts":[],"service_tiers":[]},{"provider":"custom","id":"unknown-choices"}]}
JSON
terminal_start -N --model-metadata "$root/models.json"
send '/reasoning\r'; expect 'no reasoning-effort choices'
send '/reasoning high\r'; expect 'does not advertise reasoning effort high'
send '/model gpt-5.6-luna high\r'; expect 'does not advertise reasoning effort high'
send '/fast\r'; expect 'no available tier choices'
send '/tier priority\r'; expect 'tier is not advertised'
send '/model unknown-choices high\r'; expect 'Model changed to unknown-choices'
send '/tier experimental\r'; expect 'Service tier selected: experimental'
[[ $(requests) == 0 ]]
terminal_close
