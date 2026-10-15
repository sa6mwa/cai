#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
export CAI_TEST_API_BASE_URL="$endpoint"
catalog='{"models":[{"slug":"gpt-6-luna","context_window":272000},{"slug":"account-catalog-marker","context_window":272000}]}'
config="$root/catalog.ini"
printf '[client]\ntimeout_ms = 4000\n' >"$config"
base+=(--config "$config")
catalog_plan() {
  rm -f "$root/peer/catalog_finished"
  plan "$(jq -n --argjson cat "$catalog" --argjson delay "$1" --argjson status "$2" '{catalog:$cat,catalog_delay_ms:$delay,catalog_status:$status,plan:[]}')"
}
# An explicitly configured request deadline must survive discovery-client
# construction. The previous hardcoded 1.5 seconds fails this response.
catalog_plan 1800 200
terminal_start -N
send '/model\r'; expect account-catalog-marker
absent "$term/feed" 'Model catalog unavailable'
wait_for test -s "$root/peer/catalog_finished"
# An explicit refresh preserves prior data if the provider is unavailable.
catalog_plan 0 503
send '/model\r'; expect 'Using cached model catalog'
expect 'Model catalog unavailable'; expect 'http_status: 503'
absent "$term/feed" 'HTTP request failed'
contains "$term/feed" account-catalog-marker
catalog_plan 0 200
send '/model\r'; expect account-catalog-marker
wait_for test -s "$root/peer/catalog_finished"
terminal_close
# A genuinely short configured deadline still applies, produces one notice,
# and can be retried immediately without waiting through the failure cooldown.
printf '[client]\ntimeout_ms = 100\n' >"$config"
catalog_plan 350 200
terminal_start -N
send '/model\r'; expect 'Model list unavailable'; expect 'Timeout was reached'
absent "$term/feed" 'HTTP transport failed'
[[ $(grep -c 'Model catalog unavailable' "$term/feed") == 1 ]]
contains "$term/raw" $'\033[0;91m$\033[0m'
wait_for test -s "$root/peer/catalog_finished"
catalog_plan 0 200
send '/model\r'; expect account-catalog-marker
contains "$term/raw" $'\033[0;92m$\033[0m'
terminal_close
logs
contains "$root/logs.jsonl" cai.http.transport_error
contains "$root/logs.jsonl" cai.models.catalog
