#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
peer_start
chat=false
[[ "$protocol" != chat_completions ]] || chat=true
printf '[client]\napi_protocol = %s\n' "$protocol" >"$root/protocol.ini"
base+=(--config "$root/protocol.ini")
export CAI_TEST_VOLATILE_SESSION=1 CAI_TEST_CAPACITY_WAIT=1
failure='{"error":"Selected model is at capacity","error_code":"server_is_overloaded"}'
plan "{\"chat\":$chat,\"plan\":[$failure,$failure,\"RECOVERED\"]}"
run -Nni original; ok
[[ $(cat "$root/stdout") == RECOVERED && $(requests) == 3 ]]
jq -se '.[0] == .[1] and .[1] == .[2]' "$root/peer/requests.jsonl" >/dev/null
contains "$root/stderr" 'retrying in 30 seconds (1/10)'
contains "$root/stderr" 'retrying in 60 seconds (2/10)'
# A failed attempt's preamble is not the final stdout answer.
plan "{\"chat\":$chat,\"plan\":[{\"text\":\"STALE\",\"error\":\"capacity\",\"error_code\":\"server_is_overloaded\"},\"FINAL ONLY\"]}"
run -Nni original; ok
[[ $(cat "$root/stdout") == 'FINAL ONLY' ]]
report='{"findings":[],"overall_correctness":"patch is correct","overall_explanation":"Verified.","overall_confidence_score":1}'
report_text=$(jq -n --arg text "$report" '{text:$text}')
plan "{\"chat\":$chat,\"plan\":[{\"text\":\"STALE REVIEW\",\"error\":\"capacity\",\"error_code\":\"server_is_overloaded\"},$report_text]}"
run --review uncommitted -T json; ok
jq -e '.findings == [] and .overall_explanation == "Verified."' "$root/stdout" >/dev/null
# An incomplete tool call never executes; an already completed one never repeats.
plan "{\"chat\":$chat,\"plan\":[{\"tool\":\"get_goal\",\"arguments\":{}},$failure,\"AFTER TOOL\"]}"
count=$(requests)
run -Nni original; ok
[[ $(cat "$root/stdout") == 'AFTER TOOL' && $(requests) == "$((count+3))" ]]
jq -se '.[-2] == .[-1]' "$root/peer/requests.jsonl" >/dev/null
export CAI_TEST_STEER_WORKER=1
plan "{\"chat\":$chat,\"plan\":[{\"tool\":\"run_worker\",\"arguments\":{\"instructions\":\"Inspect\"}},{\"text\":\"STALE CHILD\",\"error\":\"capacity\",\"error_code\":\"server_is_overloaded\"},\"CHILD FINAL\",\"PARENT FINAL\"]}"
run -Nni original; ok
[[ $(cat "$root/stdout") == 'PARENT FINAL' ]]
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" 'CHILD FINAL'
absent "$root/request" 'STALE CHILD'
unset CAI_TEST_STEER_WORKER
# Exhaustion ends goal pursuit too, rather than starting another paid turn.
plan "$(jq -n --argjson chat "$chat" --argjson error "$failure" \
  '{chat:$chat,plan:[range(11)|$error]}')"
count=$(requests)
run -Nn --goal 'Finish the task'; bad
[[ $(requests) == "$((count+11))" && ! -s "$root/stdout" ]]
contains "$root/stderr" 'retrying in 240 seconds (10/10)'
# All supported configuration sources can disable/re-enable the default.
for source in flag env ini; do
  unset CAI_CAPACITY_RETRY
  extra=()
  case "$source" in
    flag) extra+=(--no-capacity-retry) ;;
    env) export CAI_CAPACITY_RETRY=false ;;
    ini) printf '[agent]\ncapacity_retry=false\n' >>"$root/protocol.ini" ;;
  esac
  plan "{\"chat\":$chat,\"plan\":[$failure]}"
  count=$(requests)
  run -Nni original "${extra[@]}"; bad
  [[ $(requests) == "$((count+1))" && ! -s "$root/stdout" ]]
  absent "$root/stderr" 'retrying in'
done
export CAI_CAPACITY_RETRY=true
plan "{\"chat\":$chat,\"plan\":[$failure,\"ENV OVERRIDE\"]}"
run -Nni original; ok
[[ $(cat "$root/stdout") == 'ENV OVERRIDE' ]]
export CAI_CAPACITY_RETRY=false
plan "{\"chat\":$chat,\"plan\":[$failure,\"FLAG OVERRIDE\"]}"
run -Nni original --capacity-retry; ok
[[ $(cat "$root/stdout") == 'FLAG OVERRIDE' ]]
unset CAI_CAPACITY_RETRY CAI_TEST_CAPACITY_WAIT
# Real wait, cancelled at its retry notice: no polling through a 30s timeout.
plan "{\"chat\":$chat,\"plan\":[$failure]}"
terminal_start -N --capacity-retry
send 'original\r'; expect 'retrying in 30 seconds'
send '\033'; expect 'Turn stopped'
terminal_close
